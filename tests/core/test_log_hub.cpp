#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "nobion/core/build_info.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/env.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_match.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/signal_bus.hpp"

using namespace nobion;
namespace fs = std::filesystem;

#ifndef _WIN32
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere
#endif

namespace {

// Fresh, unique temporary directory removed at scope exit.
class TempDir {
 public:
  TempDir() : path_(unique_path()) { fs::create_directories(path_); }
  // For death tests. On Windows the child is a fresh run of the test rather
  // than a fork, so it would pick a different random directory from the one
  // the parent inspects: the parent publishes its directory in an environment
  // variable keyed by `name`, and the child adopts it. The name is never part
  // of the path, so concurrent runs of the same test (another checkout,
  // another ctest) cannot remove each other's directory.
  explicit TempDir(const std::string& name) : env_("NOBION_LOG_HUB_TMP_" + name) {
    if (const auto inherited = env_var(env_.c_str()); inherited && !inherited->empty()) {
      path_ = *inherited;
      owner_ = false;
      return;
    }
    path_ = unique_path();
    fs::create_directories(path_);
    set_env(path_.string());
  }
  ~TempDir() {
    if (!owner_) return;
    if (!env_.empty()) set_env("");
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const fs::path& path() const { return path_; }

 private:
  static fs::path unique_path() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    return fs::temp_directory_path() / ("nobion_log_hub_" + std::to_string(gen()));
  }
  // An empty value removes the variable.
  void set_env(const std::string& value) const {
#ifdef _WIN32
    _putenv_s(env_.c_str(), value.c_str());
#else
    if (value.empty()) {
      ::unsetenv(env_.c_str());
    } else {
      ::setenv(env_.c_str(), value.c_str(), 1);
    }
#endif
  }

  fs::path path_;
  std::string env_;  // empty: not shared with a death-test child
  bool owner_ = true;
};

config::LoggingConfig config_for(const fs::path& dir) {
  config::LoggingConfig cfg;
  cfg.dir = dir;
  return cfg;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> read_lines(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

// A file line without its thread field and run id, the shape these tests
// were written against: `... [info] [t3] run=X core: m` -> `... [info] core: m`.
std::string bare(const std::string& text) {
  static const std::regex context(R"((\[(?:trace|debug|info|warn|error)\]) \[[^\]\n]*\] (?:run=\S+ )?)");
  return std::regex_replace(text, context, "$1 ");
}

std::string read_bare_file(const fs::path& p) { return bare(read_file(p)); }

std::vector<std::string> read_bare_lines(const fs::path& p) {
  auto lines = read_lines(p);
  for (auto& line : lines) line = bare(line);
  return lines;
}

std::shared_ptr<LogHub> make_hub(const config::LoggingConfig& cfg, const Clock& clock,
                                 SignalBus* bus = nullptr) {
  auto hub = LogHub::create(cfg, clock, bus);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

long process_number() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

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

// tests/core/crash_helper.cpp as a child process on `dir`.
class HelperProcess {
 public:
  HelperProcess(const fs::path& dir, const std::string& mode) {
    const std::string helper = NOBION_CRASH_HELPER;
#ifdef _WIN32
    const std::wstring program = fs::path(helper).wstring();
    std::wstring command = L"\"" + program + L"\" \"" + dir.wstring() + L"\" " + fs::path(mode).wstring();
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    PROCESS_INFORMATION process{};
    const UINT previous = SetErrorMode(0);
    (void)SetErrorMode(previous | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const BOOL created = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        nullptr, &startup, &process);
    (void)SetErrorMode(previous);
    if (created == FALSE) return;
    (void)CloseHandle(process.hThread);
    process_ = process.hProcess;
    pid_ = static_cast<long>(process.dwProcessId);
#else
    std::string helper_arg = helper;
    std::string dir_arg = dir.string();
    std::string mode_arg = mode;
    std::vector<char*> argv{helper_arg.data(), dir_arg.data(), mode_arg.data(), nullptr};
    pid_t pid = -1;
    if (posix_spawn(&pid, helper.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return;
    pid_ = pid;
#endif
  }
  ~HelperProcess() { stop(); }
  HelperProcess(const HelperProcess&) = delete;
  HelperProcess& operator=(const HelperProcess&) = delete;

  [[nodiscard]] bool started() const { return pid_ > 0; }
  [[nodiscard]] long pid() const { return pid_; }

  // Ends the child where it stands (no handler runs) and waits for it.
  void stop() {
    if (pid_ <= 0) return;
#ifdef _WIN32
    (void)TerminateProcess(process_, 1);
    (void)WaitForSingleObject(process_, INFINITE);
    (void)CloseHandle(process_);
    process_ = nullptr;
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
#endif
    pid_ = 0;
  }

  // Waits (at most 30 s) for the child to end by itself.
  void wait() {
    if (pid_ <= 0) return;
#ifdef _WIN32
    if (WaitForSingleObject(process_, 30000) != WAIT_OBJECT_0) return;
    (void)CloseHandle(process_);
    process_ = nullptr;
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    while (::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == 0) {
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    pid_ = 0;
  }

 private:
  long pid_ = -1;
#ifdef _WIN32
  HANDLE process_ = nullptr;
#endif
};

// True once `path` exists, within 30 s.
bool appears(const fs::path& path) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::error_code ec;
  while (!fs::exists(path, ec)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

}  // namespace

TEST(LogHub, WritesFormattedLineToFile) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Warn, "transport.x", "hello");
  hub->flush();

  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1u);
  const std::regex re(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[warn\] \[t\d+\] transport\.x: hello$)");
  EXPECT_TRUE(std::regex_match(lines[0], re)) << lines[0];
}

TEST(LogHub, LevelNamesInFile) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Trace, "a", "t");
  hub->write(LogLevel::Debug, "a", "d");
  hub->write(LogLevel::Info, "a", "i");
  hub->write(LogLevel::Warn, "a", "w");
  hub->write(LogLevel::Error, "a", "e");
  hub->flush();

  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 5u);
  const std::array<std::string, 5> names{"trace", "debug", "info", "warn", "error"};
  const std::array<std::string, 5> messages{"t", "d", "i", "w", "e"};
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::regex re(R"(Z \[)" + names.at(i) + R"(\] \[t\d+\] a: )" + messages.at(i) + "$");
    EXPECT_TRUE(std::regex_search(lines[i], re)) << lines[i];
  }
}

TEST(LogHub, LineCarriesThreadAndRun) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.dock_level = LogLevel::Debug;
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);

  {
    const LogContext::Thread t{"executor"};
    const LogContext::Run r{"12345-01A"};
    hub->write(LogLevel::Debug, "experiment.run", "msg");
  }
  hub->flush();

  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1u);
  const std::regex re(R"(^\S+Z \[debug\] \[executor\] run=12345-01A experiment\.run: msg$)");
  EXPECT_TRUE(std::regex_match(lines[0], re)) << lines[0];
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].thread, "executor");
  EXPECT_EQ(got[0].run, "12345-01A");
  EXPECT_EQ(got[0].logger, "experiment.run");
  EXPECT_EQ(got[0].message, "msg");
}

TEST(LogHub, UntaggedThreadHasNumberNoRun) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = make_hub(config_for(tmp.path()), clock, &bus);
  ASSERT_TRUE(hub);

  std::thread worker([&hub] { hub->write(LogLevel::Info, "logger", "msg"); });
  worker.join();
  hub->flush();

  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1u);
  const std::regex re(R"(\[t\d+\] logger: msg$)");
  EXPECT_TRUE(std::regex_search(lines[0], re)) << lines[0];
  EXPECT_EQ(lines[0].find("run="), std::string::npos) << lines[0];
  ASSERT_EQ(got.size(), 1u);
  ASSERT_FALSE(got[0].thread.empty());
  EXPECT_EQ(got[0].thread.front(), 't');
  EXPECT_EQ(got[0].thread.find_first_not_of("0123456789", 1), std::string::npos) << got[0].thread;
  EXPECT_EQ(got[0].run, "");
}

TEST(LogHub, ErrorIsOnDiskWithoutFlush) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);

  hub->write(LogLevel::Error, "core", "disk on fire");

  EXPECT_NE(read_bare_file(tmp.path() / "nobion.log").find("[error] core: disk on fire"),
            std::string::npos);
}

TEST(LogHub, DestructorFlushesPending) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    for (int i = 0; i < 1000; ++i) hub->write(LogLevel::Info, "bulk", "line " + std::to_string(i));
  }
  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1000u);
  EXPECT_NE(lines.back().find("line 999"), std::string::npos);
}

TEST(LogHub, RotatesAtSizeLimit) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.max_size_mb = 1;
  cfg.max_files = 3;
  const std::string payload(200, 'x');
  {
    auto hub = make_hub(cfg, clock);
    ASSERT_TRUE(hub);
    // ~5 MB of records.
    for (int i = 0; i < 5 * 1024 * 1024 / 256; ++i) hub->write(LogLevel::Info, "rot", payload);
  }

  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(tmp.path())) {
    if (entry.path().extension() == ".lock") continue;  // the hub's lock file
    if (entry.path().filename() == "crash") continue;   // the crash folder
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"nobion.1.log", "nobion.2.log", "nobion.log"}));

  constexpr std::uintmax_t limit = 1024 * 1024 + 512;  // 1 MB + one line
  for (const auto& n : names) EXPECT_LE(fs::file_size(tmp.path() / n), limit) << n;
}

TEST(LogHub, UnwritableDirDegrades) {
  TempDir tmp;
  const fs::path regular = tmp.path() / "not_a_dir";
  { std::ofstream(regular) << "x"; }
  const fs::path bad = regular / "logs";

  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });

  auto hub = LogHub::create(config_for(bad), clock, &bus);
  ASSERT_TRUE(hub.has_value());
  EXPECT_NO_THROW((*hub)->write(LogLevel::Error, "core", "still alive"));
  EXPECT_NO_THROW((*hub)->flush());

  // The start-up report, then the record itself (bus sink).
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Error);
  EXPECT_EQ(got[0].logger, "logging");
  EXPECT_NE(got[0].message.find(bad.string()), std::string::npos) << got[0].message;
  EXPECT_EQ(got[1].message, "still alive");
  EXPECT_FALSE(fs::exists(bad));
}

// A subscriber of the bus that throws says so in the log: once, then at the
// 10th failure, the 100th...
TEST(LogHub, ABusHandlerThatThrowsIsLoggedAndNotAtEveryFailure) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());

  auto bad = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("no such valve"); });
  for (int i = 0; i < 12; ++i) bus.publish(ValveChanged{});
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Error);
  EXPECT_EQ(got[0].logger, "bus");
  EXPECT_EQ(got[0].message, "a handler of nobion::ValveChanged threw: no such valve");
  EXPECT_EQ(got[1].message, "a handler of nobion::ValveChanged threw: no such valve (10 times so far)");

  // The hub is gone: the bus goes on, and nothing is written.
  hub->reset();
  bus.publish(ValveChanged{});
  EXPECT_EQ(got.size(), 2u);
}

// The record of a failure is a Log on the same bus: a Log handler that
// throws does not make the two feed each other.
TEST(LogHub, AThrowingLogHandlerDoesNotLoop) {
  SteadyClock clock;
  SignalBus bus;
  int logs = 0;
  auto sub = bus.subscribe<Log>([&](const Log&) {
    ++logs;
    throw std::runtime_error("log handler");
  });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());
  (*hub)->write(LogLevel::Info, "core", "hello");
  // "hello", then the one record of its handler's first failure.
  EXPECT_EQ(logs, 2);
  EXPECT_EQ(bus.handler_failures(), 2u);
}

TEST(LogHub, FileNameIsUsed) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.file_name = "elctl.log";
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  hub->write(LogLevel::Warn, "core", "x");
  hub->flush();
  EXPECT_TRUE(fs::exists(tmp.path() / "elctl.log"));
  EXPECT_FALSE(fs::exists(tmp.path() / "nobion.log"));
}

TEST(LogHub, EmptyDirMeansNoFileSink) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });

  auto hub = LogHub::create(config::LoggingConfig{}, clock, &bus);
  ASSERT_TRUE(hub.has_value());
  EXPECT_NO_THROW((*hub)->write(LogLevel::Error, "core", "nowhere"));
  EXPECT_NO_THROW((*hub)->flush());
  // No start-up report; only the record itself reaches the bus.
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].logger, "core");
  EXPECT_EQ(got[0].message, "nowhere");
}

TEST(LogHub, MessagesAreVerbatim) {
  TempDir tmp;
  SteadyClock clock;
  const std::string fmt_like = "100% {} {0}";
  const std::string multiline = "line1\nline2";
  const std::string high_byte = std::string("bad\xff") + "byte";
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", fmt_like));
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", multiline));
    EXPECT_NO_THROW(hub->write(LogLevel::Info, "v", high_byte));
  }
  const std::string text = read_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("v: " + fmt_like + "\n"), std::string::npos) << text;
  EXPECT_NE(text.find("v: line1 / line2\n"), std::string::npos) << text;  // one record, one line
  EXPECT_NE(text.find("v: " + high_byte + "\n"), std::string::npos) << text;
}

// A record is one line of the file, so that every line has its time, thread
// and run: the line breaks of a message become " / ". The Log event (the
// dock) keeps the message as it was.
TEST(LogHub, AMultiLineMessageIsOneLineOfTheFile) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.echo_stderr = true;
  {
    auto hub = make_hub(cfg, clock, &bus);
    ASSERT_TRUE(hub);
    testing::internal::CaptureStderr();
    hub->write(LogLevel::Warn, "v", "a\nb");
    hub->write(LogLevel::Warn, "v", "c\r\nd\re\n");
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err, "[warn] v: a / b\n[warn] v: c / d / e / \n");
  }
  const auto lines = read_bare_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 2u) << ::testing::PrintToString(lines);
  EXPECT_TRUE(lines[0].ends_with("[warn] v: a / b")) << lines[0];
  EXPECT_TRUE(lines[1].ends_with("[warn] v: c / d / e / ")) << lines[1];
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].message, "a\nb");
  EXPECT_EQ(got[1].message, "c\r\nd\re\n");
}

// ---- Task 4: hub-backed Logger -------------------------------------------

namespace {

config::LoggingConfig rules_config(std::vector<std::pair<std::string, LogLevel>> levels,
                                   LogLevel default_level) {
  config::LoggingConfig cfg;
  cfg.default_level = default_level;
  cfg.levels = std::move(levels);
  cfg.recorder.enabled = false;  // these tests are about the file level; enabled() also counts the recorder
  return cfg;
}

// rules_config with a ring, for tests that move the recorder's levels (with
// no ring they stay off).
config::LoggingConfig recorder_rules_config(std::vector<std::pair<std::string, LogLevel>> levels,
                                            LogLevel default_level) {
  config::LoggingConfig cfg = rules_config(std::move(levels), default_level);
  cfg.recorder.enabled = true;
  cfg.recorder.size_mb = static_cast<std::size_t>(config::kRecorderMinMb);
  return cfg;
}

}  // namespace

TEST(LogHub, LevelFromRules) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"transport.*", LogLevel::Debug},
                                    {"transport.serial.*", LogLevel::Trace}},
                                   LogLevel::Warn),
                      clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->logger("transport.serial.ig1").level(), LogLevel::Trace);
  EXPECT_EQ(hub->logger("transport.tcp").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("core").level(), LogLevel::Warn);
  EXPECT_EQ(hub->logger("transport").level(), LogLevel::Warn);  // glob needs a suffix
}

TEST(Logger, LevelStaysTheFileLevel) {
  SteadyClock clock;
  auto hub = make_hub(recorder_rules_config({{"*.wire", LogLevel::Info}}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  hub->set_recorder_levels(LogLevel::Debug, {{"*.wire", LogLevel::Trace}});
  const Logger wire = hub->logger("ig1.wire");
  EXPECT_EQ(wire.level(), LogLevel::Info);
  EXPECT_EQ(wire.recorder_level(), LogLevel::Trace);
  EXPECT_TRUE(wire.enabled(LogLevel::Trace));
  const Logger core = hub->logger("core");
  EXPECT_EQ(core.level(), LogLevel::Info);
  EXPECT_EQ(core.recorder_level(), LogLevel::Debug);
  EXPECT_TRUE(core.enabled(LogLevel::Debug));
  EXPECT_FALSE(core.enabled(LogLevel::Trace));
}

TEST(Logger, ExplicitLevelLeavesTheRecorderAlone) {
  SteadyClock clock;
  auto hub = make_hub(recorder_rules_config({}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  hub->set_recorder_levels(LogLevel::Debug, {{"*.wire", LogLevel::Trace}});
  Logger wire = hub->logger("ig1.wire");
  ASSERT_EQ(wire.recorder_level(), LogLevel::Trace);
  wire.set_level(LogLevel::Error);
  EXPECT_EQ(wire.level(), LogLevel::Error);
  EXPECT_EQ(wire.recorder_level(), LogLevel::Trace);
  hub->set_recorder_levels(LogLevel::Warn, {});  // a rule change still moves it
  EXPECT_EQ(wire.recorder_level(), LogLevel::Warn);
  EXPECT_EQ(wire.level(), LogLevel::Error);
}

TEST(Logger, ARecorderRuleChangeDoesNotMoveTheFileLevel) {
  SteadyClock clock;
  auto hub = make_hub(recorder_rules_config({}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  const Logger log = hub->logger("core");
  ASSERT_EQ(log.level(), LogLevel::Warn);
  hub->set_recorder_levels(LogLevel::Trace, {});
  EXPECT_EQ(log.level(), LogLevel::Warn);
  EXPECT_EQ(log.recorder_level(), LogLevel::Trace);
}

TEST(LogHub, ARecordBelowTheFileLevelIsNotInTheFile) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;
  cfg.dock_level = LogLevel::Trace;
  cfg.echo_stderr = true;
  cfg.levels = {{"*.wire", LogLevel::Info}};
  cfg.recorder.levels = {{"*.wire", LogLevel::Trace}};
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);

  const Logger wire = hub->logger("ig1.wire");
  testing::internal::CaptureStderr();
  wire.trace("below the file level");
  wire.info("at the file level");
  hub->flush();
  const std::string err = testing::internal::GetCapturedStderr();

  const auto text = read_file(tmp.path() / "nobion.log");
  EXPECT_EQ(text.find("below the file level"), std::string::npos) << text;
  EXPECT_NE(text.find("at the file level"), std::string::npos) << text;
  EXPECT_EQ(err.find("below the file level"), std::string::npos) << err;
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].message, "at the file level");
}

TEST(LogHub, ADisabledRecorderKeepsNothingBelowTheFileLevel) {
  SteadyClock clock;
  auto cfg = rules_config({}, LogLevel::Info);
  cfg.recorder.enabled = false;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  const Logger log = hub->logger("core");
  EXPECT_FALSE(log.enabled(LogLevel::Debug));
  EXPECT_TRUE(log.enabled(LogLevel::Info));
}

// With no ring there is nothing for a recorder level to keep: a rule change
// leaves it off, so nothing is formatted for nothing.
TEST(LogHub, RecorderLevelsChangeNothingWithoutARing) {
  SteadyClock clock;
  auto cfg = rules_config({}, LogLevel::Info);
  cfg.recorder.enabled = false;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  const Logger log = hub->logger("core");
  hub->set_recorder_levels(LogLevel::Trace, {});
  EXPECT_FALSE(log.enabled(LogLevel::Debug));
  EXPECT_TRUE(log.enabled(LogLevel::Info));
}

// record() reaches the ring alone; log_unrecorded() then reaches the file,
// stderr and bus, and the ring holds the line once.
TEST(Logger, RecordThenLogUnrecordedKeepsTheLineOnceInTheRing) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;
  cfg.dock_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);
  const Logger log = hub->logger("hang");

  log.record(LogLevel::Error, "the one line");
  hub->flush();
  EXPECT_EQ(read_file(tmp.path() / "nobion.log").find("the one line"), std::string::npos);
  EXPECT_TRUE(got.empty());
  const std::string ring = hub->recorder_snapshot();
  EXPECT_NE(ring.find("[error] ["), std::string::npos) << ring;
  EXPECT_NE(ring.find("hang: the one line"), std::string::npos) << ring;

  log.log_unrecorded(LogLevel::Error, "the one line");
  EXPECT_NE(read_file(tmp.path() / "nobion.log").find("the one line"), std::string::npos);  // an error: on disk now
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].message, "the one line");
  const std::string after = hub->recorder_snapshot();
  EXPECT_EQ(after.find("the one line"), after.rfind("the one line")) << after;
}

TEST(LogHub, BareNameRuleMatchesChildren) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"scheduler", LogLevel::Debug}}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->logger("scheduler").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("scheduler.jobs").level(), LogLevel::Debug);
  EXPECT_EQ(hub->logger("schedulerx").level(), LogLevel::Info);
}

TEST(LogHub, RuleOrderIndependent) {
  SteadyClock clock;
  const std::pair<std::string, LogLevel> a{"transport.*", LogLevel::Debug};
  const std::pair<std::string, LogLevel> b{"transport.serial.*", LogLevel::Error};
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  for (const char* name : {"transport.serial.ig1", "transport.tcp", "core"}) {
    EXPECT_EQ(h1->logger(name).level(), h2->logger(name).level()) << name;
  }
  EXPECT_EQ(h1->logger("transport.serial.ig1").level(), LogLevel::Error);
}

TEST(LogHub, EqualSpecificityTieIsDeterministic) {
  SteadyClock clock;
  // Same specificity (2 literals, one star) and same length: the
  // lexicographically greater pattern ("a*b" > "*ab") wins, in either order.
  const std::pair<std::string, LogLevel> a{"a*b", LogLevel::Trace};
  const std::pair<std::string, LogLevel> b{"*ab", LogLevel::Error};
  ASSERT_EQ(log_rule_specificity(a.first), log_rule_specificity(b.first));
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  EXPECT_EQ(h1->logger("aab").level(), LogLevel::Trace);
  EXPECT_EQ(h2->logger("aab").level(), LogLevel::Trace);
}

TEST(LogHub, LongerPatternBreaksSpecificityTie) {
  SteadyClock clock;
  // Both have 2 literals and a star; "a**b" is longer than "a*b".
  const std::pair<std::string, LogLevel> a{"a*b", LogLevel::Error};
  const std::pair<std::string, LogLevel> b{"a**b", LogLevel::Trace};
  ASSERT_EQ(log_rule_specificity(a.first), log_rule_specificity(b.first));
  auto h1 = make_hub(rules_config({a, b}, LogLevel::Info), clock);
  auto h2 = make_hub(rules_config({b, a}, LogLevel::Info), clock);
  ASSERT_TRUE(h1 && h2);
  EXPECT_EQ(h1->logger("axb").level(), LogLevel::Trace);
  EXPECT_EQ(h2->logger("axb").level(), LogLevel::Trace);
}

TEST(LogHub, SetLevelReresolvesLiveLoggers) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  Logger copy = log;
  EXPECT_FALSE(log.enabled(LogLevel::Trace));
  EXPECT_FALSE(copy.enabled(LogLevel::Trace));

  hub->set_level("core", LogLevel::Trace);
  EXPECT_TRUE(log.enabled(LogLevel::Trace));
  EXPECT_TRUE(copy.enabled(LogLevel::Trace));
  EXPECT_EQ(log.level(), LogLevel::Trace);
  EXPECT_TRUE(hub->logger("core").enabled(LogLevel::Trace));
  EXPECT_TRUE(hub->logger("core.sub").enabled(LogLevel::Trace));
  EXPECT_FALSE(hub->logger("other").enabled(LogLevel::Trace));
}

// What File > Preferences applies: the whole table at once, so a rule taken
// out of it stops applying.
TEST(LogHub, SetLevelsReplacesEveryRuleAndTheDefault) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"core", LogLevel::Trace}, {"scheduler", LogLevel::Debug}}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger core = hub->logger("core");
  Logger scheduler = hub->logger("scheduler");
  Logger other = hub->logger("other");
  EXPECT_TRUE(core.enabled(LogLevel::Trace));
  EXPECT_FALSE(other.enabled(LogLevel::Info));

  hub->set_levels(LogLevel::Info, {{"scheduler", LogLevel::Error}});
  EXPECT_FALSE(core.enabled(LogLevel::Trace)) << "the rule for core was taken out";
  EXPECT_TRUE(core.enabled(LogLevel::Info));
  EXPECT_TRUE(other.enabled(LogLevel::Info)) << "the default changed";
  EXPECT_FALSE(scheduler.enabled(LogLevel::Warn));
  EXPECT_TRUE(scheduler.enabled(LogLevel::Error));
  EXPECT_TRUE(hub->logger("later").enabled(LogLevel::Info));
}

TEST(LogHub, ChildResolvesItsOwnLevel) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({{"systems.gauges", LogLevel::Trace}}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger parent = hub->logger("systems");
  Logger child = parent.child("gauges");
  EXPECT_EQ(child.name(), "systems.gauges");
  EXPECT_EQ(parent.level(), LogLevel::Warn);
  EXPECT_EQ(child.level(), LogLevel::Trace);
}

TEST(LogHub, ExplicitLoggerLevelSticks) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Warn), clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  log.set_level(LogLevel::Error);
  hub->set_level("core", LogLevel::Trace);
  EXPECT_EQ(log.level(), LogLevel::Error);
  EXPECT_FALSE(log.enabled(LogLevel::Warn));
}

TEST(LogHub, PublishesBusEventsWithClockTs) {
  using namespace std::chrono_literals;
  ManualClock clock;
  clock.advance(5s);
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock, &bus);
  ASSERT_TRUE(hub);

  Logger log = hub->logger("switches");
  log.debug("hidden");
  log.info("valve A opened");
  log.error("valve B failed");

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Info);
  EXPECT_EQ(got[0].logger, "switches");
  EXPECT_EQ(got[0].message, "valve A opened");
  EXPECT_EQ(got[0].ts, clock.now());
  EXPECT_EQ(got[1].level, LogLevel::Error);
  EXPECT_EQ(got[1].message, "valve B failed");
}

TEST(LogHub, LoggerWritesToFile) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  Logger log = hub->logger("core");
  log.debug("hidden");
  log.warn("shown");
  hub->flush();
  const auto lines = read_bare_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find(" [warn] core: shown"), std::string::npos) << lines[0];
}

TEST(LogHub, EchoesToStderr) {
  SteadyClock clock;
  auto cfg = rules_config({}, LogLevel::Info);
  cfg.echo_stderr = true;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  testing::internal::CaptureStderr();
  hub->logger("core").warn("careful");
  hub->logger("core").debug("hidden");
  const std::string err = testing::internal::GetCapturedStderr();
  EXPECT_EQ(err, "[warn] core: careful\n");
}

TEST(LogHub, NoStderrEchoByDefault) {
  SteadyClock clock;
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);
  testing::internal::CaptureStderr();
  hub->logger("core").warn("careful");
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

TEST(LogHub, SubscriberMayLogBack) {
  // Bus publishing runs on the caller thread, so a subscriber that logs an
  // error (which waits for the writer) cannot deadlock against the writer.
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(config_for(tmp.path()), clock, &bus);
  ASSERT_TRUE(hub);
  Logger relay = hub->logger("relay");
  int seen = 0;
  auto sub = bus.subscribe<Log>([&](const Log& e) {
    ++seen;
    if (e.logger != "relay") relay.error("saw " + e.message);
  });
  hub->logger("core").error("boom");
  EXPECT_EQ(seen, 2);
  EXPECT_NE(read_bare_file(tmp.path() / "nobion.log").find("[error] relay: saw boom"),
            std::string::npos);
}

TEST(LogHub, LoggerOutlivesHub) {
  SteadyClock clock;
  SignalBus bus;
  int seen = 0;
  auto sub = bus.subscribe<Log>([&](const Log&) { ++seen; });
  std::optional<Logger> survivor;
  {
    auto hub = make_hub(rules_config({}, LogLevel::Info), clock, &bus);
    ASSERT_TRUE(hub);
    survivor.emplace(hub->logger("core"));
  }
  EXPECT_NO_THROW(survivor->error("x"));
  EXPECT_NO_THROW(survivor->set_level(LogLevel::Trace));
  EXPECT_EQ(survivor->level(), LogLevel::Trace);
  EXPECT_NO_THROW(survivor->trace("y"));
  Logger child = survivor->child("sub");
  EXPECT_NO_THROW(child.error("z"));
  EXPECT_EQ(seen, 0);
}

TEST(LogHub, ConcurrentWriteAndSetLevel) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::atomic<int> bus_errors{0};
  auto sub = bus.subscribe<Log>([&](const Log& e) {
    if (e.level == LogLevel::Error) bus_errors.fetch_add(1);
  });
  auto hub = make_hub(config_for(tmp.path()), clock, &bus);
  ASSERT_TRUE(hub);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      Logger log = hub->logger("core.w" + std::to_string(t));
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < kPerThread; ++i) {
        log.debug("d");
        log.error("e " + std::to_string(i));
      }
    });
  }
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 1000; ++i) {
      hub->set_level("core", (i % 2 == 0) ? LogLevel::Trace : LogLevel::Error);
    }
  });
  // The dock level moves under the writers too; error records reach the bus whatever it is.
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 1000; ++i) hub->set_dock_level((i % 2 == 0) ? LogLevel::Trace : LogLevel::Error);
  });
  go.store(true);
  for (auto& th : threads) th.join();
  hub->flush();

  EXPECT_EQ(bus_errors.load(), kThreads * kPerThread);
  int file_errors = 0;
  for (const auto& line : read_bare_lines(tmp.path() / "nobion.log")) {
    if (line.find("[error] core.w") != std::string::npos) ++file_errors;
  }
  EXPECT_EQ(file_errors, kThreads * kPerThread);
}

// ---- Attachments, dock level, file lock ------------------------------------

namespace {

// Calendar instant from a UTC date and time, for ManualClock epochs.
WallTime utc(int y, unsigned mo, unsigned d, int h, int mi, int s) {
  using namespace std::chrono;
  return sys_days{year{y} / month{mo} / day{d}} + hours{h} + minutes{mi} + seconds{s};
}

std::string current_pid() {
#ifdef _WIN32
  return std::to_string(_getpid());
#else
  return std::to_string(process_number());
#endif
}

}  // namespace

TEST(LogHub, DockLevelFiltersBusNotFile) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Debug;
  cfg.dock_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);

  hub->logger("core").debug("quiet");
  hub->logger("core").info("loud");
  hub->flush();

  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].message, "loud");
  const auto text = read_bare_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("[debug] core: quiet"), std::string::npos) << text;
  EXPECT_NE(text.find("[info] core: loud"), std::string::npos) << text;
}

TEST(LogHub, SetDockLevelChangesWhatTheBusGetsAtOnce) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  config::LoggingConfig cfg;  // no file: the bus alone
  cfg.default_level = LogLevel::Trace;
  cfg.dock_level = LogLevel::Info;
  auto hub = make_hub(cfg, clock, &bus);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->dock_level(), LogLevel::Info);

  const Logger wire = hub->logger("transport.serial.ig1.wire");
  wire.trace("before");
  hub->set_dock_level(LogLevel::Trace);
  EXPECT_EQ(hub->dock_level(), LogLevel::Trace);
  wire.trace("after");
  hub->set_dock_level(LogLevel::Warn);
  wire.info("quiet again");
  wire.warn("loud");

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].message, "after");
  EXPECT_EQ(got[1].message, "loud");
}

TEST(LogHub, SessionLineSaysTheDockLevelInForce) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    hub->set_dock_level(LogLevel::Debug);
    hub->begin_session(SessionInfo{"test", "0", std::nullopt, "", false, "steady"});
  }
  const auto text = read_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("dock_level=debug"), std::string::npos) << text;
}

TEST(LogHub, AttachBusLater) {
  TempDir tmp;
  SteadyClock clock;
  SignalBus bus;
  std::vector<std::string> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e.message); });
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->bus(), nullptr);

  hub->write(LogLevel::Info, "core", "a");
  {
    const LogHub::BusAttachment attached = hub->attach_bus(bus);
    EXPECT_EQ(hub->bus(), &bus);
    hub->write(LogLevel::Info, "core", "b");
  }
  EXPECT_EQ(hub->bus(), nullptr);
  hub->write(LogLevel::Info, "core", "c");
  hub->flush();

  EXPECT_EQ(got, (std::vector<std::string>{"b"}));
  const auto lines = read_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find("core: a"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find("core: b"), std::string::npos) << lines[1];
  EXPECT_NE(lines[2].find("core: c"), std::string::npos) << lines[2];
}

// A handler of an attached bus that throws is logged, as with a bus given to
// create(); once detached, its failures are not.
TEST(LogHub, AttachedBusHandlerFailuresAreLogged) {
  SteadyClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto bad = bus.subscribe<ValveChanged>([](const ValveChanged&) { throw std::runtime_error("no valve"); });
  auto hub = make_hub(config::LoggingConfig{}, clock);
  ASSERT_TRUE(hub);
  {
    const LogHub::BusAttachment attached = hub->attach_bus(bus);
    bus.publish(ValveChanged{});
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].logger, "bus");
  bus.publish(ValveChanged{});
  EXPECT_EQ(got.size(), 1u);
}

TEST(LogHub, DetachWaitsForInFlightPublish) {
  SteadyClock clock;
  SignalBus bus;
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> first{true};
  auto sub = bus.subscribe<Log>([&](const Log&) {
    if (!first.exchange(false)) return;
    entered.set_value();
    released.wait();
  });
  auto hub = make_hub(rules_config({}, LogLevel::Info), clock);
  ASSERT_TRUE(hub);

  std::optional<LogHub::BusAttachment> attached;
  attached.emplace(hub->attach_bus(bus));
  std::thread writer([&] { hub->write(LogLevel::Info, "core", "held"); });
  const bool publishing =
      entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  EXPECT_TRUE(publishing);

  std::atomic<bool> detached{false};
  std::promise<void> done;
  std::thread detacher([&] {
    attached.reset();
    detached.store(true);
    done.set_value();
  });
  // The publish is still blocked in the handler: the detach must not return.
  for (int i = 0; i < 10000 && !detached.load(); ++i) std::this_thread::yield();
  EXPECT_FALSE(detached.load());

  release.set_value();
  EXPECT_EQ(done.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  writer.join();
  detacher.join();
  EXPECT_TRUE(detached.load());
  EXPECT_EQ(hub->bus(), nullptr);
}

TEST(LogHub, ClockDetachFallsBackToHubClock) {
  using namespace std::chrono_literals;
  TempDir tmp;
  const ManualClock a(TimePoint{} + 10s, utc(2020, 1, 1, 0, 0, 0));
  const ManualClock b(TimePoint{} + 99s, utc(2026, 10, 22, 14, 3, 22));
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  auto hub = make_hub(config_for(tmp.path()), a, &bus);
  ASSERT_TRUE(hub);
  {
    const LogHub::ClockAttachment attached = hub->attach_clock(b);
    hub->write(LogLevel::Info, "core", "on b");
  }
  hub->write(LogLevel::Info, "core", "on a");
  hub->flush();

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].ts, b.now());
  EXPECT_EQ(got[1].ts, a.now());
  const auto lines = read_bare_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "2026-10-22T14:03:22.000Z [info] core: on b");
  EXPECT_EQ(lines[1], "2020-01-01T00:00:00.000Z [info] core: on a");
}

TEST(LogHub, SecondProcessGetsPidFile) {
  TempDir tmp;
  SteadyClock clock;
  auto first = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->file_path(), tmp.path() / "nobion.log");

  auto second = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(second);
  const fs::path pid_file = tmp.path() / ("nobion-" + current_pid() + ".log");
  EXPECT_EQ(second->file_path(), pid_file);
  second->flush();

  const auto lines = read_bare_lines(pid_file);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[info] logging: "), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find("in use"), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find((tmp.path() / "nobion.log").string()), std::string::npos) << lines[0];
}

TEST(LogHub, LockReleasedOnDestruction) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    EXPECT_EQ(hub->file_path(), tmp.path() / "nobion.log");
  }
  auto again = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->file_path(), tmp.path() / "nobion.log");
}

// A lock that cannot be taken for a reason other than another holder (here
// the lock path is a directory) keeps the main name and says so at warn.
TEST(LogHub, UnlockableFileIsAWarning) {
  TempDir tmp;
  fs::create_directories(tmp.path() / "nobion.log.lock");
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  EXPECT_EQ(hub->file_path(), tmp.path() / "nobion.log");
  hub->flush();
  const auto lines = read_bare_lines(tmp.path() / "nobion.log");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[warn] logging: cannot lock "), std::string::npos) << lines[0];
}

// Attachments made, replaced and dropped while other threads write: every
// record reaches the file, no bus hears a record after its attachment's
// destructor has returned, and every stamp comes from one of the clocks.
TEST(LogHub, AttachAndDropUnderConcurrentWrites) {
  using namespace std::chrono_literals;
  TempDir tmp;
  SteadyClock base;
  const ManualClock c1(TimePoint{}, utc(2001, 1, 1, 0, 0, 0));
  const ManualClock c2(TimePoint{}, utc(2002, 1, 1, 0, 0, 0));
  auto hub = make_hub(config_for(tmp.path()), base);
  ASSERT_TRUE(hub);
  Logger relay = hub->logger("relay");

  // A bus is "live" from before its attach until its attachment's
  // destructor has returned; an event outside that is a violation.
  struct Watched {
    SignalBus bus;
    std::atomic<bool> live{false};
    std::atomic<int> seen{0};
    std::atomic<int> late{0};
    SignalBus::Subscription sub;
  };
  Watched b1;
  Watched b2;
  for (Watched* w : {&b1, &b2}) {
    w->sub = w->bus.subscribe<Log>([w, &relay](const Log& e) {
      if (!w->live.load()) w->late.fetch_add(1);
      w->seen.fetch_add(1);
      if (e.logger != "relay") relay.info("saw " + e.logger);  // logs back
    });
  }

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 1000;
  constexpr int kMinRounds = 50;
  std::atomic<int> writers_left{kWriters};
  std::atomic<int> bus_rounds{0};
  std::atomic<int> clock_rounds{0};
  std::promise<void> all_done;
  std::vector<std::thread> threads;
  threads.reserve(kWriters + 2);

  for (int t = 0; t < kWriters; ++t) {
    threads.emplace_back([&, t] {
      Logger log = hub->logger("w" + std::to_string(t));
      for (int i = 0; i < kPerWriter; ++i) log.info(std::to_string(i));
      writers_left.fetch_sub(1);
    });
  }
  threads.emplace_back([&] {
    while (writers_left.load() > 0 || bus_rounds.load() < kMinRounds) {
      b1.live.store(true);
      std::optional<LogHub::BusAttachment> a1(hub->attach_bus(b1.bus));
      b2.live.store(true);
      std::optional<LogHub::BusAttachment> a2(hub->attach_bus(b2.bus));  // replaces b1
      a2.reset();
      b2.live.store(false);
      a1.reset();
      b1.live.store(false);
      bus_rounds.fetch_add(1);
    }
  });
  threads.emplace_back([&] {
    while (writers_left.load() > 0 || clock_rounds.load() < kMinRounds) {
      std::optional<LogHub::ClockAttachment> k1(hub->attach_clock(c1));
      std::optional<LogHub::ClockAttachment> k2(hub->attach_clock(c2));
      k1.reset();  // replaced already: changes nothing
      k2.reset();
      clock_rounds.fetch_add(1);
    }
  });
  std::thread joiner([&] {
    for (auto& th : threads) th.join();
    all_done.set_value();
  });
  if (all_done.get_future().wait_for(30s) != std::future_status::ready) {
    ADD_FAILURE() << "attach/drop under writes did not finish in 30 s";
    std::abort();
  }
  joiner.join();
  hub->flush();

  EXPECT_EQ(b1.late.load(), 0);
  EXPECT_EQ(b2.late.load(), 0);
  EXPECT_EQ(hub->bus(), nullptr);
  // Whether any write fell inside an attachment above is the scheduler's
  // (a fast Release build finished every write first): delivery is checked
  // here, after the race.
  {
    const int before = b1.seen.load();
    b1.live.store(true);
    const LogHub::BusAttachment attached = hub->attach_bus(b1.bus);
    relay.info("after the race");
    hub->flush();
    EXPECT_GT(b1.seen.load(), before);
  }
  b1.live.store(false);

  std::vector<int> per_writer(kWriters, 0);
  const std::regex stamp(R"(^(\d{4})-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[)");
  const int this_year = [] {
    const auto days = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return static_cast<int>(std::chrono::year_month_day(days).year());
  }();
  for (const auto& line : read_bare_lines(tmp.path() / "nobion.log")) {
    std::smatch m;
    ASSERT_TRUE(std::regex_search(line, m, stamp)) << line;
    const int year = std::stoi(m[1].str());
    EXPECT_TRUE(year == 2001 || year == 2002 || year == this_year || year == this_year - 1) << line;
    for (int t = 0; t < kWriters; ++t) {
      if (line.find("[info] w" + std::to_string(t) + ": ") != std::string::npos) ++per_writer[t];
    }
  }
  for (int t = 0; t < kWriters; ++t) EXPECT_EQ(per_writer[t], kPerWriter) << "writer " << t;
}

TEST(LogHub, NoFileMeansEmptyFilePath) {
  SteadyClock clock;
  auto hub = make_hub(config::LoggingConfig{}, clock);
  ASSERT_TRUE(hub);
  EXPECT_TRUE(hub->file_path().empty());
}

// --- Crash and terminate handlers (spec 4.5) -------------------------------
//
// Each death test runs a child (a fork in "fast" style; TempDir(name) hands
// it the parent's directory either way) that builds its own hub, crashes, and
// dies; the parent then inspects nobion.log.

namespace {

std::size_t count_lines_containing(const fs::path& p, const std::string& needle) {
  const auto lines = read_bare_lines(p);
  return static_cast<std::size_t>(std::count_if(
      lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
}

// Not inline-visible to the noexcept caller, so no "will always terminate"
// warning; the throw escapes a noexcept frame and reaches std::terminate with
// the exception active.
[[noreturn]] void throw_boom() { throw std::runtime_error("boom"); }
void (*volatile g_throw_boom)() = &throw_boom;
void call_noexcept_throw() noexcept { g_throw_boom(); }

int g_prev_terminate_calls = 0;
[[noreturn]] void counting_terminate() {
  ++g_prev_terminate_calls;
  std::fprintf(stderr, "prev terminate calls=%d\n", g_prev_terminate_calls);
  std::fflush(stderr);
  std::_Exit(3);
}

}  // namespace

TEST(LogHubCrash, TerminateFlushesQueuedRecordsAndExceptionText) {
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("TerminateFlushesQueuedRecordsAndExceptionText");
  SteadyClock clock;
  EXPECT_DEATH(
      {
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        auto log = (*hub)->logger("crash.test");
        for (int i = 0; i < 200; ++i) log.info("record " + std::to_string(i));
        call_noexcept_throw();
      },
      "");
  const auto file = tmp.path() / "nobion.log";
  EXPECT_EQ(count_lines_containing(file, "[info] crash.test: record "), 200u);
  EXPECT_GE(count_lines_containing(file, "boom"), 1u);
}

TEST(LogHubCrash, SigabrtLeavesFatalSignalLine) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("SigabrtLeavesFatalSignalLine");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        // Installed before the hub exists: create() opens the descriptor.
        LogHub::install_crash_handlers();
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        (*hub)->logger("crash.test").error("about to abort");
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  const auto file = tmp.path() / "nobion.log";
  EXPECT_EQ(count_lines_containing(file, "[error] crash.test: about to abort"), 1u);
  EXPECT_EQ(count_lines_containing(file, "fatal signal 6"), 1u);
#endif
}

TEST(LogHubCrash, SignalGuaranteeIsBounded) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  // Documents the 1 s periodic-flush bound only: an info record older than
  // that survives a hard crash. Nothing asserts younger sub-error records do.
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("SignalGuaranteeIsBounded");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        (*hub)->logger("crash.test").info("old enough");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  const auto file = tmp.path() / "nobion.log";
  EXPECT_EQ(count_lines_containing(file, "[info] crash.test: old enough"), 1u);
  EXPECT_EQ(count_lines_containing(file, "fatal signal 6"), 1u);
#endif
}

TEST(LogHubCrash, FatalSignalLineFollowsRotation) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX signal handlers";
#else
  // The crash descriptor is opened on the nobion.log of the moment; once the
  // file rotates, the next flusher tick must move it onto the new live file.
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("FatalSignalLineFollowsRotation");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        auto cfg = config_for(tmp.path());
        cfg.max_size_mb = 1;
        cfg.max_files = 3;
        auto hub = LogHub::create(cfg, clock);
        if (!hub) std::_Exit(10);
        LogHub::install_crash_handlers();
        auto log = (*hub)->logger("crash.test");
        const std::string filler(1000, 'x');
        for (int i = 0; i < 1500; ++i) log.info(filler);  // > 1 MiB: rotates
        (*hub)->flush();
        if (!fs::exists(tmp.path() / "nobion.1.log")) std::_Exit(11);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // >= 2 flusher ticks
        std::abort();
      },
      ::testing::KilledBySignal(SIGABRT), "fatal signal 6");
  EXPECT_EQ(count_lines_containing(tmp.path() / "nobion.log", "fatal signal 6"), 1u);
  EXPECT_EQ(count_lines_containing(tmp.path() / "nobion.1.log", "fatal signal 6"), 0u);
#endif
}

TEST(LogHubCrash, InstallIsIdempotent) {
  GTEST_FLAG_SET(death_test_style, "fast");
  TempDir tmp("InstallIsIdempotent");
  SteadyClock clock;
  EXPECT_EXIT(
      {
        std::set_terminate(&counting_terminate);
        LogHub::install_crash_handlers();
        LogHub::install_crash_handlers();
        auto hub = LogHub::create(config_for(tmp.path()), clock);
        if (!hub) std::_Exit(10);
        std::terminate();
      },
      ::testing::ExitedWithCode(3), "prev terminate calls=1\n");
  // No active exception: the terminate route still logs a line.
  EXPECT_EQ(count_lines_containing(tmp.path() / "nobion.log", "terminate"), 1u);
}

TEST(LogHub, SessionMarkerFields) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    SessionInfo info;
    info.app = "elctl";
    info.version = "0.4.0";
    info.clock = "steady";
    hub->begin_session(info);
  }
  const auto text = read_bare_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("[info] nobion: session start "), std::string::npos) << text;
  for (const char* field : {"app=elctl", "version=0.4.0", "install=none", "profiles=none", "sim=no",
                            "clock=steady", "default_level=debug", "dock_level=info", "levels=none"}) {
    EXPECT_NE(text.find(field), std::string::npos) << field << " in " << text;
  }
  EXPECT_NE(text.find("os="), std::string::npos) << text;
}

TEST(LogHub, SessionStartNamesTheBuild) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    SessionInfo info;
    info.app = "elctl";
    info.version = "0.4.0";
    info.clock = "steady";
    hub->begin_session(info);
  }
  const auto text = read_bare_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("build=" + nobion::build_id()), std::string::npos) << text;
}

TEST(LogHub, SessionMarkerQuotesSpacesAndListsRules) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    hub->set_level("transport", LogLevel::Warn);
    SessionInfo info;
    info.app = "nobion";
    info.version = "0.4.0";
    info.install_root = fs::path("/tmp/my install");
    info.profiles = "a,b";
    info.simulated = true;
    info.clock = "virtual x10";
    hub->begin_session(info);
  }
  const auto text = read_bare_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("install=\"/tmp/my install\""), std::string::npos) << text;
  EXPECT_NE(text.find("profiles=a,b"), std::string::npos) << text;
  EXPECT_NE(text.find("sim=yes"), std::string::npos) << text;
  EXPECT_NE(text.find("clock=\"virtual x10\""), std::string::npos) << text;
  EXPECT_NE(text.find("levels=transport:warn"), std::string::npos) << text;
}

TEST(LogHub, SessionEndOnDestruction) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    SessionInfo info;
    info.app = "elctl";
    hub->begin_session(info);
    hub->write(LogLevel::Info, "x", "work");
  }
  const auto lines = read_bare_lines(tmp.path() / "nobion.log");
  ASSERT_GE(lines.size(), 3u);
  const std::string tail = "nobion: session end";
  EXPECT_EQ(lines.back().substr(lines.back().size() - tail.size()), tail) << lines.back();
}

TEST(LogHub, NoSessionNoEndLine) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    hub->write(LogLevel::Info, "x", "work");
  }
  EXPECT_EQ(read_file(tmp.path() / "nobion.log").find("session"), std::string::npos);
}

// Session sentinel (spec 2026-11-06-field-diagnostics, 3.3).

TEST(LogHub, ACleanEndLeavesNoSentinel) {
  TempDir tmp;
  SteadyClock clock;
  {
    auto hub = make_hub(config_for(tmp.path()), clock);
    ASSERT_TRUE(hub);
    SessionInfo info;
    info.app = "nobion";
    hub->begin_session(info);
    EXPECT_TRUE(fs::exists(tmp.path() / "nobion.session"));
    EXPECT_FALSE(hub->previous_session());
  }
  EXPECT_FALSE(fs::exists(tmp.path() / "nobion.session"));
  auto again = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(again);
  EXPECT_FALSE(again->previous_session());
  again->flush();
  EXPECT_EQ(read_file(tmp.path() / "nobion.log").find("without a clean exit"), std::string::npos);
}

TEST(LogHub, AKilledSessionIsReportedOnce) {
  TempDir tmp;
  SteadyClock clock;
  HelperProcess helper(tmp.path(), "segv");
  ASSERT_TRUE(helper.started()) << "could not spawn " << NOBION_CRASH_HELPER;
  const long helper_pid = helper.pid();
  helper.wait();
  ASSERT_TRUE(fs::exists(tmp.path() / "crash_helper.session"));

  auto cfg = config_for(tmp.path());
  cfg.file_name = "crash_helper.log";
  {
    auto hub = make_hub(cfg, clock);
    ASSERT_TRUE(hub);
    const auto previous = hub->previous_session();
    ASSERT_TRUE(previous);
    EXPECT_EQ(previous->build_id, build_id());
    EXPECT_FALSE(previous->started.empty());
    if (!kAddressSanitizer) {
      ASSERT_TRUE(previous->crash_file);
      EXPECT_EQ(previous->crash_file->filename().string(), "crash_helper-" + std::to_string(helper_pid) + ".txt");
    }
    hub->flush();
    const std::string text = read_file(tmp.path() / "crash_helper.log");
    EXPECT_NE(text.find("[error]"), std::string::npos) << text;
    EXPECT_NE(text.find("previous crash_helper session " + build_id() + " started"), std::string::npos) << text;
    EXPECT_NE(text.find("ended without a clean exit"), std::string::npos) << text;
    EXPECT_FALSE(fs::exists(tmp.path() / "crash_helper.session"));
  }
  auto third = make_hub(cfg, clock);
  ASSERT_TRUE(third);
  EXPECT_FALSE(third->previous_session());
}

TEST(LogHub, ALiveSecondProcessIsNeverReportedUnclean) {
  TempDir tmp;
  SteadyClock clock;
  HelperProcess helper(tmp.path(), "sleep");
  ASSERT_TRUE(helper.started()) << "could not spawn " << NOBION_CRASH_HELPER;
  ASSERT_TRUE(appears(tmp.path() / "crash_helper.session"));

  auto cfg = config_for(tmp.path());
  cfg.file_name = "crash_helper.log";
  {
    auto hub = make_hub(cfg, clock);
    ASSERT_TRUE(hub);
    EXPECT_FALSE(hub->previous_session());
    SessionInfo info;
    info.app = "test";
    hub->begin_session(info);  // its own sentinel, crash_helper-<pid>.session
    EXPECT_TRUE(fs::exists(tmp.path() / ("crash_helper-" + std::to_string(process_number()) + ".session")));
    EXPECT_TRUE(fs::exists(tmp.path() / "crash_helper.session"));
    hub->flush();
    EXPECT_EQ(read_file(hub->file_path()).find("without a clean exit"), std::string::npos);
  }
  EXPECT_TRUE(fs::exists(tmp.path() / "crash_helper.session"));
  helper.stop();
  auto after = make_hub(cfg, clock);
  ASSERT_TRUE(after);
  EXPECT_TRUE(after->previous_session());  // now that it is gone, it is reported
}

namespace {

// A sentinel no process holds, as a process that is gone left it.
void write_stale_sentinel(const fs::path& path, const std::string& pid) {
  std::ofstream out(path, std::ios::binary);
  out << "pid=" << pid << "\nbuild=abc123\nstarted=2026-11-06T14:03:22Z\n";
}

void write_crash_file(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << "fatal signal 11\n";
}

}  // namespace

// Programs share a log folder: the crash file is the sentinel's program's,
// never the stem of the hub that reports it.
TEST(LogHub, AnotherProgramsSentinelNamesThatProgramsCrashFile) {
  TempDir tmp;
  SteadyClock clock;
  write_stale_sentinel(tmp.path() / "elctl.session", "4242");
  write_crash_file(tmp.path() / "crash" / "elctl-4242.txt");
  write_crash_file(tmp.path() / "crash" / "nobion-4242.txt");  // the reporting hub's stem: not it

  auto hub = make_hub(config_for(tmp.path()), clock);  // nobion.log
  ASSERT_TRUE(hub);
  const auto previous = hub->previous_session();
  ASSERT_TRUE(previous);
  EXPECT_EQ(previous->program, "elctl");
  ASSERT_TRUE(previous->crash_file);
  EXPECT_EQ(previous->crash_file->filename().string(), "elctl-4242.txt");
  hub->flush();
  const std::string text = read_file(tmp.path() / "nobion.log");
  EXPECT_NE(text.find("previous elctl session abc123 started 2026-11-06T14:03:22Z ended without a clean exit"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("elctl-4242.txt"), std::string::npos) << text;
}

// A second hub's sentinel is `<stem>-<pid>.session`; its crash file is
// still `<stem>-<pid>.txt`, and its program the stem without the pid.
TEST(LogHub, ASecondHubsSentinelDropsItsPidFromTheProgram) {
  TempDir tmp;
  SteadyClock clock;
  write_stale_sentinel(tmp.path() / "elctl-4242.session", "4242");
  write_crash_file(tmp.path() / "crash" / "elctl-4242.txt");

  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const auto previous = hub->previous_session();
  ASSERT_TRUE(previous);
  EXPECT_EQ(previous->program, "elctl");
  ASSERT_TRUE(previous->crash_file);
  EXPECT_EQ(previous->crash_file->filename().string(), "elctl-4242.txt");
}

// A pid comes round again: a crash file older than the session is an
// earlier process's, not this session's.
TEST(LogHub, ACrashFileOlderThanTheSessionIsNotItsCrash) {
  TempDir tmp;
  SteadyClock clock;
  const fs::path sentinel = tmp.path() / "nobion.session";
  const fs::path crash = tmp.path() / "crash" / "nobion-4242.txt";
  write_stale_sentinel(sentinel, "4242");
  write_crash_file(crash);
  fs::last_write_time(crash, fs::last_write_time(sentinel) - std::chrono::hours(1));

  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const auto previous = hub->previous_session();
  ASSERT_TRUE(previous);
  EXPECT_FALSE(previous->crash_file) << previous->crash_file->string();
  hub->flush();
  EXPECT_EQ(read_file(tmp.path() / "nobion.log").find("crash file"), std::string::npos);
}

// A crash file written within the slack before the sentinel still counts
// (file times are coarse on some file systems).
TEST(LogHub, ACrashFileWithinTheSlackIsTheSessionsCrash) {
  TempDir tmp;
  SteadyClock clock;
  const fs::path sentinel = tmp.path() / "nobion.session";
  const fs::path crash = tmp.path() / "crash" / "nobion-4242.txt";
  write_stale_sentinel(sentinel, "4242");
  write_crash_file(crash);
  fs::last_write_time(crash, fs::last_write_time(sentinel) - std::chrono::seconds(2));

  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const auto previous = hub->previous_session();
  ASSERT_TRUE(previous);
  EXPECT_TRUE(previous->crash_file);
}

// The sentinel's start time is the hub's clock's wall time (a simulated
// session is stamped in simulated time), not the system's.
TEST(LogHub, TheSentinelIsStampedFromTheHubsClock) {
  TempDir tmp;
  const ManualClock clock(TimePoint{},
                          std::chrono::sys_days(std::chrono::year{2001} / 1 / 2) + std::chrono::hours(3));
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  SessionInfo info;
  info.app = "nobion";
  hub->begin_session(info);
  const std::string text = read_file(tmp.path() / "nobion.session");
  EXPECT_NE(text.find("started=2001-01-02T03:00:00Z\n"), std::string::npos) << text;
}

// ---- Flight recorder (spec 2026-11-06-field-diagnostics, 5.2-5.4) ---------

namespace {

// What the dump limits are measured on, moved by hand.
struct LimitClock {
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  void attach(LogHub& hub) {
    hub.set_recorder_limit_clock_for_testing([this] { return now; });
  }
};

std::vector<fs::path> dumps_in(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir / "flight", ec), end; !ec && it != end; it.increment(ec)) {
    out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// A wire record below the file level is in the ring, in the file's line
// format, and not in the file; a record that goes to the file is in both,
// the same bytes.
TEST(LogHub, SnapshotHoldsWireTraceNotInTheFile) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;
  cfg.recorder.default_level = LogLevel::Debug;
  cfg.recorder.levels = {{"*.wire", LogLevel::Trace}};
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);

  const Logger wire = hub->logger("ig1.wire");
  const Logger core = hub->logger("core");
  wire.trace("tx 0a 0d");
  core.info("in both");
  hub->flush();

  const std::string file = read_file(tmp.path() / "nobion.log");
  const std::string snap = hub->recorder_snapshot();
  EXPECT_EQ(file.find("tx 0a 0d"), std::string::npos) << file;
  EXPECT_EQ(wire.level(), LogLevel::Info);  // the file level is unchanged

  const std::regex wire_line(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[trace\] \[[^\]]+\] ig1\.wire: tx 0a 0d$)");
  bool found = false;
  std::istringstream in(snap);
  for (std::string line; std::getline(in, line);) found = found || std::regex_match(line, wire_line);
  EXPECT_TRUE(found) << snap;

  std::string both;
  for (const auto& line : read_lines(tmp.path() / "nobion.log")) {
    if (line.find("in both") != std::string::npos) both = line;
  }
  ASSERT_FALSE(both.empty()) << file;
  EXPECT_NE(snap.find(both + "\n"), std::string::npos) << "file line: " << both << "\nsnapshot:\n" << snap;
  // Oldest first.
  EXPECT_LT(snap.find("tx 0a 0d"), snap.find("in both")) << snap;
}

TEST(LogHub, ADisabledRecorderHasNothingToDump) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.recorder.enabled = false;
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  hub->logger("core").info("hello");
  EXPECT_EQ(hub->recorder_snapshot(), "");
  const auto dumped = hub->dump_recorder("manual");
  ASSERT_FALSE(dumped);
  EXPECT_EQ(dumped.error().kind, ErrorKind::Config);
  EXPECT_TRUE(dumps_in(tmp.path()).empty());
}

TEST(LogHub, DumpWritesReasonAndBuildFirst) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  hub->logger("core").info("before the dump");
  const auto dumped = hub->dump_recorder("manual");
  ASSERT_TRUE(dumped) << dumped.error().what;
  EXPECT_EQ(dumped->parent_path(), tmp.path() / "flight");
  EXPECT_TRUE(dumped->filename().string().ends_with("-manual.log")) << *dumped;
  const auto lines = read_lines(*dumped);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front(), "flight recorder dump: reason=manual build=" + build_id());
  EXPECT_NE(read_file(*dumped).find("core: before the dump\n"), std::string::npos);
}

TEST(LogHub, ADumpNameIsStampedFromTheHubsClockAndKeepsItsReasonSafe) {
  TempDir tmp;
  const ManualClock clock(TimePoint{}, std::chrono::sys_days(std::chrono::year{2001} / 1 / 2) +
                                           std::chrono::hours(3) + std::chrono::milliseconds(45));
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const auto dumped = hub->dump_recorder("alarm-ig1/gauge:1");
  ASSERT_TRUE(dumped) << dumped.error().what;
  EXPECT_EQ(dumped->filename().string(), "20010102T030000.045Z-alarm-ig1_gauge_1.log");
  EXPECT_EQ(read_lines(*dumped).front(), "flight recorder dump: reason=alarm-ig1/gauge:1 build=" + build_id());
}

// Control characters (a line break above all) never reach the dump's first
// line or its name, and the name keeps at most 64 bytes of the reason.
TEST(LogHub, ADumpsReasonLosesControlCharactersAndIsCappedInTheName) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const std::string longest(100, 'a');
  const auto dumped = hub->dump_recorder("alarm\nforged line\x01\t" + longest);
  ASSERT_TRUE(dumped) << dumped.error().what;
  const std::string name = dumped->filename().string();
  const std::string stem = name.substr(name.find('-') + 1);  // after the stamp
  EXPECT_EQ(stem, ("alarmforged_line" + longest).substr(0, 64) + ".log") << name;
  const auto lines = read_lines(*dumped);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines.front(), "flight recorder dump: reason=alarmforged line" + longest + " build=" + build_id());
}

TEST(LogHub, SameReasonWithinAMinuteWritesNothing) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  LimitClock limit;
  limit.attach(*hub);

  const auto first = hub->dump_recorder("manual");
  ASSERT_TRUE(first) << first.error().what;
  limit.now += std::chrono::seconds(59);
  const auto again = hub->dump_recorder("manual");
  ASSERT_TRUE(again) << again.error().what;
  EXPECT_EQ(*again, *first);  // the last dump's path
  EXPECT_EQ(dumps_in(tmp.path()).size(), 1u);
  hub->flush();
  EXPECT_NE(read_file(tmp.path() / "nobion.log").find("flight recorder dump for reason=manual skipped"),
            std::string::npos);

  limit.now += std::chrono::seconds(2);
  const auto later = hub->dump_recorder("manual");
  ASSERT_TRUE(later) << later.error().what;
  EXPECT_NE(*later, *first);
  EXPECT_EQ(dumps_in(tmp.path()).size(), 2u);
}

TEST(LogHub, AnyReasonWithinTenSecondsWritesNothing) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  LimitClock limit;
  limit.attach(*hub);

  const auto first = hub->dump_recorder("alarm-ig1");
  ASSERT_TRUE(first) << first.error().what;
  limit.now += std::chrono::seconds(9);
  const auto other = hub->dump_recorder("unsafe");
  ASSERT_TRUE(other) << other.error().what;
  EXPECT_EQ(*other, *first);
  EXPECT_EQ(dumps_in(tmp.path()).size(), 1u);

  limit.now += std::chrono::seconds(2);
  const auto later = hub->dump_recorder("unsafe");
  ASSERT_TRUE(later) << later.error().what;
  EXPECT_TRUE(later->filename().string().ends_with("-unsafe.log")) << *later;
  EXPECT_EQ(dumps_in(tmp.path()).size(), 2u);
}

TEST(LogHub, KeepsTheNewestTwentyDumps) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  LimitClock limit;
  limit.attach(*hub);

  std::vector<fs::path> written;
  for (int i = 0; i < 25; ++i) {
    const std::string reason = std::string("r") + (i < 10 ? "0" : "") + std::to_string(i);
    const auto dumped = hub->dump_recorder(reason);
    ASSERT_TRUE(dumped) << dumped.error().what;
    written.push_back(*dumped);
    limit.now += std::chrono::seconds(11);
  }
  EXPECT_EQ(dumps_in(tmp.path()).size(), 20u);
  for (std::size_t i = 0; i < written.size(); ++i) {
    EXPECT_EQ(fs::exists(written[i]), i >= 5) << written[i];
  }
}

// Review Focus 4: a dump that cannot be written is an Io error that leaves
// no file, and logging goes on.
TEST(LogHub, ADumpToAReadOnlyDirectoryFailsCleanly) {
  TempDir tmp;
  SteadyClock clock;
  auto hub = make_hub(config_for(tmp.path()), clock);
  ASSERT_TRUE(hub);
  const fs::path flight = tmp.path() / "flight";
#ifdef _WIN32
  // A read-only bit does not stop writes into a folder on Windows: a file
  // where the folder should be does.
  std::ofstream(flight) << "in the way";
#else
  if (::geteuid() == 0) GTEST_SKIP() << "root writes into a read-only folder";
  fs::create_directories(flight);
  fs::permissions(flight, fs::perms::owner_read | fs::perms::owner_exec);
#endif
  const auto dumped = hub->dump_recorder("manual");
#ifndef _WIN32
  const auto entries = dumps_in(tmp.path());
  fs::permissions(flight, fs::perms::owner_all);
  EXPECT_TRUE(entries.empty());
#endif
  ASSERT_FALSE(dumped);
  EXPECT_EQ(dumped.error().kind, ErrorKind::Io);

  hub->logger("core").info("still logging");
  hub->flush();
  EXPECT_NE(read_file(tmp.path() / "nobion.log").find("still logging"), std::string::npos);
  // A failed dump sets no limit: the next one is tried.
  fs::remove_all(flight);
  const auto retried = hub->dump_recorder("manual");
  EXPECT_TRUE(retried) << retried.error().what;
}

TEST(LogHub, NoFileNoDump) {
  SteadyClock clock;
  config::LoggingConfig cfg;
  cfg.dir = fs::path();
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);
  hub->logger("core").info("in memory only");
  EXPECT_NE(hub->recorder_snapshot().find("core: in memory only\n"), std::string::npos);
  const auto dumped = hub->dump_recorder("manual");
  ASSERT_FALSE(dumped);
  EXPECT_EQ(dumped.error().kind, ErrorKind::Io);
}

// Writers on several threads while snapshots and dumps are taken, enough
// bytes to wrap the ring several times: every line of every snapshot is
// whole and one the test wrote (a snapshot is taken under the ring's lock, so
// not even its oldest line may be torn). (Run under TSan, root AGENTS.md.)
TEST(LogHub, ConcurrentWritesSnapshotsAndDumps) {
  TempDir tmp;
  SteadyClock clock;
  auto cfg = config_for(tmp.path());
  cfg.default_level = LogLevel::Info;  // the writers' debug records go to the ring only
  cfg.recorder.default_level = LogLevel::Debug;
  cfg.recorder.size_mb = static_cast<std::size_t>(config::kRecorderMinMb);
  auto hub = make_hub(cfg, clock);
  ASSERT_TRUE(hub);

  constexpr int kWriters = 4;
  constexpr int kRecords = 6000;
  const std::string pad(900, 'x');
  // About 4 x 6000 x 960 bytes: over five times the 4 MiB ring.
  static_assert(static_cast<std::size_t>(kWriters) * kRecords * 900 >
                5 * static_cast<std::size_t>(config::kRecorderMinMb) * 1024 * 1024);
  std::atomic<int> done{0};
  std::atomic<int> last_to_finish{-1};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int t = 0; t < kWriters; ++t) {
    writers.emplace_back([&, t] {
      const Logger log = hub->logger("w" + std::to_string(t));
      for (int i = 0; i < kRecords; ++i) log.debug("record " + std::to_string(i) + " " + pad + " end");
      last_to_finish.store(t);
      done.fetch_add(1);
    });
  }

  const std::regex prefix(R"(^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z \[(debug|info)\] \[t\d+\] )");
  const std::regex writer_body(R"(^w\d: record \d+ $)");  // before the pad and " end"
  const std::regex dump_body(R"(^nobion: flight recorder dump for reason=k (skipped \(.*\); last dump |written to ).+$)");
  const std::string tail = " " + pad + " end";
  auto check = [&](const std::string& line) {
    std::smatch m;
    if (!std::regex_search(line, m, prefix)) return false;
    const std::string body = m.suffix().str();
    if (body.ends_with(tail)) return std::regex_match(body.substr(0, body.size() - tail.size() + 1), writer_body);
    return std::regex_match(body, dump_body);
  };
  int snapshots = 0;
  do {  // at least once, however soon the writers finish
    const std::string snap = hub->recorder_snapshot();
    ++snapshots;
    if (!snap.empty()) {
      EXPECT_EQ(snap.back(), '\n');
      EXPECT_LE(snap.size(), static_cast<std::size_t>(config::kRecorderMinMb) * 1024 * 1024);
      std::istringstream in(snap);
      for (std::string line; std::getline(in, line);) {
        if (!check(line)) {
          ADD_FAILURE() << "not a line the test wrote: " << line.substr(0, 200);
          break;
        }
      }
    }
    (void)hub->dump_recorder("k");
  } while (done.load() < kWriters);
  for (auto& w : writers) w.join();
  EXPECT_GT(snapshots, 0);
  // The ring wrapped: the first records are gone, and the last record written
  // is kept. Which writer wrote it rests on scheduling: a writer starved under
  // load can finish more than the ring's size behind the others, and a writer
  // that finished early has had its last record pushed out. The writer that
  // finished last wrote the newest record, so that one is asked for.
  const std::string last = hub->recorder_snapshot();
  EXPECT_EQ(last.find("] w0: record 0 "), std::string::npos);
  const int newest = last_to_finish.load();
  ASSERT_GE(newest, 0);
  EXPECT_NE(last.find("] w" + std::to_string(newest) + ": record " + std::to_string(kRecords - 1) + " "),
            std::string::npos);
}
