// A program that dies on purpose, for tests/core/test_crash_capture.cpp.
//
//   crash_helper <log dir> <mode>
//
// Makes a hub on <log dir> (file crash_helper.log), installs the crash
// handlers, logs `ring marker` at info, then acts by <mode>:
//   sleep            blocks until killed (the session sentinel tests of test_log_hub.cpp)
//   segv             writes through a null pointer
//   abort            std::abort()
//   bus fpe ill trap raise(SIGBUS) and so on (POSIX)
//   overflow-main    unbounded recursion on the main thread
//   overflow-thread  the same on a thread that called install_signal_stack()
//   raw-thread-segv  a null write on a raw std::thread with no signal stack
//   hardened         std::vector<int>(1)[5]; exits 77 unless the standard
//                    library is hardened (has_feature("hardened_stdlib"))
//   terminate        std::terminate() inside the catch of a
//                    std::runtime_error("crash helper boom")
// The main thread's log tag is `crash_helper.main`; overflow-thread's thread
// is `crash_helper.overflow`; raw-thread-segv's thread has none.
// Exit status 2: bad arguments; 3: the hub could not be made; 4: survived.

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "nobion/core/build_info.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/signal_stack.hpp"

#if defined(_MSC_VER)
#define CRASH_HELPER_NOINLINE __declspec(noinline)
#else
#define CRASH_HELPER_NOINLINE __attribute__((noinline))
#endif

namespace {

volatile int g_sink = 0;  // keeps the compiler from proving anything about the faults below

// Recursion the optimiser cannot turn into a loop: each frame keeps a buffer
// alive across the call and reads it after.
CRASH_HELPER_NOINLINE int recurse(int depth) {
  std::array<volatile char, 512> frame{};
  frame[0] = static_cast<char>(depth);
  const int below = g_sink >= 0 ? recurse(depth + 1) : 0;
  return below + frame[0];
}

CRASH_HELPER_NOINLINE void write_null() {
  int* volatile target = nullptr;
  // cppcheck-suppress nullPointer ; the crash is the point
  *target = 1;  // NOLINT(clang-analyzer-core.NullDereference): the crash is the point
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: crash_helper <log dir> <mode>\n");
    return 2;
  }
  const std::string_view mode = argv[2];
  const nobion::LogContext::Thread tag("crash_helper.main");

  nobion::SteadyClock clock;
  nobion::config::LoggingConfig config;
  config.dir = std::filesystem::path(argv[1]);
  config.file_name = "crash_helper.log";
  config.default_level = nobion::LogLevel::Info;
  auto hub = nobion::LogHub::create(config, clock);
  if (!hub) return 3;
  nobion::LogHub::install_crash_handlers();
  nobion::SessionInfo session;
  session.app = "crash_helper";
  session.clock = "steady";
  (*hub)->begin_session(session);  // makes the session sentinel (Task 7)
  (*hub)->logger("crash_helper").info("ring marker");

  if (mode == "sleep") {
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  if (mode == "segv") write_null();
  if (mode == "abort") std::abort();
#ifndef _WIN32
  if (mode == "bus") std::raise(SIGBUS);
  if (mode == "trap") std::raise(SIGTRAP);
#endif
  if (mode == "fpe") std::raise(SIGFPE);
  if (mode == "ill") std::raise(SIGILL);
  if (mode == "overflow-main") g_sink = recurse(0);
  if (mode == "overflow-thread") {
    std::thread t([] {
      nobion::install_signal_stack();
      const nobion::LogContext::Thread thread_tag("crash_helper.overflow");
      g_sink = recurse(0);
    });
    t.join();
  }
  if (mode == "raw-thread-segv") {
    std::thread t([] { write_null(); });
    t.join();
  }
  if (mode == "terminate") {
    try {
      throw std::runtime_error("crash helper boom");
    } catch (...) {
      std::terminate();
    }
  }
  if (mode == "hardened") {
    if (!nobion::has_feature("hardened_stdlib")) return 77;
    std::vector<int> v(1);
    const std::size_t index = g_sink >= 0 ? 5 : 0;
    g_sink = v[index];
  }
  return 4;
}
