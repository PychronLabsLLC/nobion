// The crash file a fatal signal leaves (spec 2026-11-06-field-diagnostics
// sections 3.2, 3.3, 14). Each test spawns tests/core/crash_helper.cpp, which
// dies the way its mode says, and reads what the handler wrote.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "nobion/core/build_info.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/signal_stack.hpp"

#ifndef _WIN32
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere
#endif

namespace fs = std::filesystem;

namespace {

// gcc says __SANITIZE_ADDRESS__; clang answers __has_feature, which a
// compiler without it must not see called (nor cppcheck, which cannot
// evaluate it).
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature) && !defined(__CPPCHECK__)
#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizer = true;
#else
constexpr bool kAddressSanitizer = false;
#endif
#else
constexpr bool kAddressSanitizer = false;
#endif

class TempDir {
 public:
  TempDir() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    path_ = fs::temp_directory_path() / ("nobion_crash_" + std::to_string(gen()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

#ifndef _WIN32

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

struct Death {
  pid_t pid = -1;
  int status = 0;
};

// Spawns the helper on `dir` in `mode` and waits (at most 30 s) for it to end.
Death run_helper(const fs::path& dir, const std::string& mode) {
  Death death;
  const std::string helper = NOBION_CRASH_HELPER;
  const std::string dir_text = dir.string();
  // posix_spawn takes char* const[]; it does not write through them.
  std::string helper_arg = helper;
  std::string dir_arg = dir_text;
  std::string mode_arg = mode;
  std::vector<char*> argv{helper_arg.data(), dir_arg.data(), mode_arg.data(), nullptr};
  if (posix_spawn(&death.pid, helper.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
    death.pid = -1;
    return death;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    const pid_t done = ::waitpid(death.pid, &death.status, WNOHANG);
    if (done == death.pid) return death;
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(death.pid, SIGKILL);
      ::waitpid(death.pid, &death.status, 0);
      ADD_FAILURE() << "helper in mode " << mode << " still running after 30 s";
      return death;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// What every crash file must hold, for a helper that died by one of
// `signals`; with `thread` set, the thread line is `thread <thread>`. The
// file's lines are left in `lines`. (void: ASSERT_* returns from it.)
void check_crash_file(const std::string& mode, std::initializer_list<int> signals, const std::string& thread,
                      std::vector<std::string>& lines) {
  TempDir tmp;
  const Death death = run_helper(tmp.path(), mode);
  ASSERT_GT(death.pid, 0) << "could not spawn " << NOBION_CRASH_HELPER;
  ASSERT_TRUE(WIFSIGNALED(death.status))
      << mode << ": exited with status " << (WIFEXITED(death.status) ? WEXITSTATUS(death.status) : -1);
  const int sig = WTERMSIG(death.status);
  bool expected = false;
  for (const int s : signals) expected = expected || s == sig;
  EXPECT_TRUE(expected) << mode << ": died by signal " << sig;

  const fs::path file = tmp.path() / "crash" / ("crash_helper-" + std::to_string(death.pid) + ".txt");
  ASSERT_TRUE(fs::exists(file)) << file;
  const std::string text = read_file(file);
  lines = lines_of(text);

  std::size_t frames = 0;
  bool signal_line = false;
  bool build_line = false;
  bool thread_line = false;
  bool helper_module = false;
  const std::regex frame_re("^0x[0-9a-f]+$");
  const std::regex module_re("^module (.+) 0x[0-9a-f]+ 0x[0-9a-f]+$");
  for (const auto& line : lines) {
    if (line == "fatal signal " + std::to_string(sig)) signal_line = true;
    if (line == "build " + nobion::build_id()) build_line = true;
    if (thread.empty() ? line.starts_with("thread ") : line == "thread " + thread) thread_line = true;
    if (std::regex_match(line, frame_re)) ++frames;
    std::smatch m;
    if (std::regex_match(line, m, module_re)) {
      if (m[1].str().ends_with("crash_helper")) helper_module = true;
    }
  }
  EXPECT_TRUE(signal_line) << text;
  EXPECT_TRUE(build_line) << text;
  EXPECT_TRUE(thread_line) << (thread.empty() ? std::string("a thread line") : "thread " + thread) << "\n" << text;
  EXPECT_GE(frames, 3u) << text;
  EXPECT_TRUE(helper_module) << text;
}

std::vector<std::string> expect_crash_file(const std::string& mode, std::initializer_list<int> signals,
                                           const std::string& thread = {}) {
  std::vector<std::string> lines;
  check_crash_file(mode, signals, thread, lines);
  return lines;
}

#endif

#ifdef _WIN32
#define CRASH_TEST_PRECONDITIONS() GTEST_SKIP() << "POSIX crash handler; Windows writes a dump (Task 6)"
#else
#define CRASH_TEST_PRECONDITIONS()                                                       \
  do {                                                                                   \
    if (kAddressSanitizer) GTEST_SKIP() << "AddressSanitizer owns the fatal signals"; \
  } while (false)
#endif

}  // namespace

#ifndef _WIN32

TEST(CrashCapture, SegvWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("segv", {SIGSEGV, SIGBUS}, "crash_helper.main");
}

TEST(CrashCapture, AbortWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("abort", {SIGABRT});
}

TEST(CrashCapture, BusWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("bus", {SIGBUS});
}

TEST(CrashCapture, FpeWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("fpe", {SIGFPE});
}

TEST(CrashCapture, IllWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("ill", {SIGILL});
}

TEST(CrashCapture, TrapWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("trap", {SIGTRAP});
}

// Stack overflow: macOS reports the guard page as SIGSEGV or SIGBUS.
TEST(CrashCapture, OverflowMainWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("overflow-main", {SIGSEGV, SIGBUS});
}

TEST(CrashCapture, OverflowThreadWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("overflow-thread", {SIGSEGV, SIGBUS}, "crash_helper.overflow");
}

// A thread that never called install_signal_stack(): SA_ONSTACK with no
// alternate stack runs the handler on the thread's own.
TEST(CrashCapture, RawThreadSegvWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  expect_crash_file("raw-thread-segv", {SIGSEGV, SIGBUS}, "?");
}

// The exception text is one line, before the SIGABRT block abort() adds.
TEST(CrashCapture, TerminateWritesTheExceptionText) {
  CRASH_TEST_PRECONDITIONS();
  const auto lines = expect_crash_file("terminate", {SIGABRT}, "crash_helper.main");
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front(), "terminate called after throwing: crash helper boom");
  EXPECT_EQ(std::count(lines.begin(), lines.end(), lines.front()), 1);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_EQ(lines[1], "fatal signal " + std::to_string(SIGABRT));
}

TEST(CrashCapture, HardenedWritesACrashFile) {
  CRASH_TEST_PRECONDITIONS();
  if (!nobion::has_feature("hardened_stdlib")) GTEST_SKIP() << "standard library not hardened";
  expect_crash_file("hardened", {SIGABRT, SIGTRAP, SIGILL});
}

#endif

TEST(CrashCapture, CrashFilePathIsInTheCrashFolder) {
  TempDir tmp;
  nobion::SteadyClock clock;
  nobion::config::LoggingConfig config;
  config.dir = tmp.path();
  config.file_name = "app.log";
  auto hub = nobion::LogHub::create(config, clock);
  ASSERT_TRUE(hub);
  EXPECT_TRUE(fs::is_directory(tmp.path() / "crash"));
#ifdef _WIN32
  const std::string pid = std::to_string(_getpid());
#else
  const std::string pid = std::to_string(::getpid());
#endif
  EXPECT_EQ((*hub)->crash_file_path(), tmp.path() / "crash" / ("app-" + pid + ".txt"));
}

TEST(CrashCapture, NoFileNoCrashFile) {
  nobion::SteadyClock clock;
  nobion::config::LoggingConfig config;
  config.dir = fs::path();
  auto hub = nobion::LogHub::create(config, clock);
  ASSERT_TRUE(hub);
  EXPECT_TRUE((*hub)->crash_file_path().empty());
}

// Hub start keeps the newest 20 crashes; a dump and its text file are one.
TEST(CrashCapture, HubStartKeepsTheNewest20Crashes) {
  TempDir tmp;
  const fs::path crash = tmp.path() / "crash";
  fs::create_directories(crash);
  const auto now = fs::file_time_type::clock::now();
  for (int i = 0; i < 25; ++i) {
    const auto stem = "app-" + std::to_string(1000 + i);
    for (const char* ext : {".txt", ".dmp"}) {
      const fs::path file = crash / (stem + ext);
      std::ofstream(file) << "crash " << i;
      fs::last_write_time(file, now - std::chrono::minutes(100 - i));
    }
  }
  std::ofstream(crash / "notes.md") << "not a crash file";

  nobion::SteadyClock clock;
  nobion::config::LoggingConfig config;
  config.dir = tmp.path();
  config.file_name = "app.log";
  auto hub = nobion::LogHub::create(config, clock);
  ASSERT_TRUE(hub);
  for (int i = 0; i < 25; ++i) {
    const auto stem = "app-" + std::to_string(1000 + i);
    const bool kept = i >= 5;  // the five oldest go
    EXPECT_EQ(fs::exists(crash / (stem + ".txt")), kept) << stem;
    EXPECT_EQ(fs::exists(crash / (stem + ".dmp")), kept) << stem;
  }
  EXPECT_TRUE(fs::exists(crash / "notes.md"));
}

TEST(CrashCapture, InstallSignalStackIsIdempotent) {
  std::thread t([] {
    nobion::install_signal_stack();
#ifndef _WIN32
    stack_t first{};
    ASSERT_EQ(::sigaltstack(nullptr, &first), 0);
    EXPECT_EQ(first.ss_flags & SS_DISABLE, 0);
    if (!kAddressSanitizer) {  // ASan may have given the thread a smaller one
      EXPECT_GE(first.ss_size, 64u * 1024u);
    }
    nobion::install_signal_stack();
    stack_t second{};
    ASSERT_EQ(::sigaltstack(nullptr, &second), 0);
    EXPECT_EQ(second.ss_sp, first.ss_sp);
#endif
  });
  t.join();
}
