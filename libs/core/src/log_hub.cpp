#include "nobion/core/log_hub.hpp"

#include "nobion/core/build_info.hpp"
#include "nobion/core/log_dir.hpp"
#include "nobion/core/signal_stack.hpp"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// dbghelp.h after windows.h, which it needs. Older SDKs' dbghelp.h trips
// C4091 (a typedef that declares nothing).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4091)
#endif
#include <dbghelp.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#else
#include <execinfo.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

#include "log_hub_internal.hpp"
#include "module_map.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_match.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion {
namespace {

constexpr std::size_t kQueueSize = 8192;
constexpr std::size_t kWriterThreads = 1;
constexpr auto kPeriodicFlush = std::chrono::seconds(1);
constexpr auto kErrorReportInterval = std::chrono::seconds(10);
constexpr std::size_t kSpdlogMaxRotated = 200000;  // spdlog's rotating_file_sink::MaxFiles
constexpr auto kCrashFlushTimeout = std::chrono::seconds(2);
constexpr std::size_t kCrashesKept = 20;  // crash files (and dumps) in <dir>/crash

// The echo and the writer's error reports go to the C stream, not through
// std::cerr: a program that tees std::cerr into its log (elctl) would log every
// echoed record again, and a report from the writer thread would wait on the
// writer. std::cerr is synchronised with stdio, so the order of lines holds.
void write_stderr(std::string_view text) noexcept {
  (void)std::fwrite(text.data(), 1, text.size(), stderr);
}

// Appends `message` with each line break ("\r\n", "\n" or "\r") written as
// " / ": a record is one line of the file, so every line carries its time,
// thread and run (grep for a run finds all of it). The Log event keeps the
// message as it was.
void append_one_line(std::string& out, std::string_view message) {
  for (std::size_t i = 0; i < message.size(); ++i) {
    const char c = message[i];
    if (c == '\r' || c == '\n') {
      if (c == '\r' && i + 1 < message.size() && message[i + 1] == '\n') ++i;
      out.append(" / ");
    } else {
      out.push_back(c);
    }
  }
}

// Set on the spdlog writer thread; the crash route must never wait on it from
// that thread (it would be waiting on itself).
thread_local bool t_is_log_writer = false;

spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Warn: return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
  }
  return spdlog::level::err;
}

// "%*": our level names ("warn", not spdlog's "warning").
class LevelFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& msg, const std::tm&,
              spdlog::memory_buf_t& dest) override {
    std::string_view name = "error";
    switch (msg.level) {
      case spdlog::level::trace: name = "trace"; break;
      case spdlog::level::debug: name = "debug"; break;
      case spdlog::level::info: name = "info"; break;
      case spdlog::level::warn: name = "warn"; break;
      default: break;
    }
    dest.append(name.data(), name.data() + name.size());
  }
  std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<LevelFlag>();
  }
};

// `2026-10-01T14:03:22.481Z [warn] transport.serial.ig1: msg`; the payload
// already carries "<logger>: <message>".
std::unique_ptr<spdlog::formatter> make_file_formatter() {
  // Explicit "\n": spdlog's default end of line is "\r\n" on Windows.
  auto f = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc, "\n");
  f->add_flag<LevelFlag>('*').set_pattern("%Y-%m-%dT%H:%M:%S.%eZ [%*] %v");
  return f;
}

// Counts barrier records. The writer thread drains the queue in FIFO order, so
// once it reaches a barrier every record enqueued before it has been sunk.
class BarrierSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  void wait_for(std::uint64_t target) {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return reached_ >= target; });
  }
  bool wait_until(std::uint64_t target, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(m_);
    return cv_.wait_until(lock, deadline, [&] { return reached_ >= target; });
  }

 protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    {
      std::lock_guard lock(m_);
      ++reached_;
    }
    cv_.notify_all();
  }
  void flush_() override {}

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::uint64_t reached_ = 0;
};

// Advisory exclusive lock on a file, held from a successful acquire() until
// release() or destruction. POSIX flock() and Windows LockFileEx both lock per
// open file description, so a second lock in the same process is refused too.
class FileLock {
 public:
  enum class Outcome { Held, Busy, Failed };

  FileLock() = default;
  ~FileLock() { release(); }
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;

  // `create` false: the file must exist already (Failed if not).
  Outcome acquire(const std::filesystem::path& path, bool create = true) noexcept {
    release();
#ifdef _WIN32
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Outcome::Failed;
    OVERLAPPED overlapped{};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
      handle_ = h;
      return Outcome::Held;
    }
    const DWORD error = GetLastError();
    CloseHandle(h);
    return error == ERROR_LOCK_VIOLATION ? Outcome::Busy : Outcome::Failed;
#else
    const int fd = ::open(path.c_str(), (create ? O_RDWR | O_CREAT : O_RDWR) | O_CLOEXEC, 0644);
    if (fd < 0) return Outcome::Failed;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      fd_ = fd;
      return Outcome::Held;
    }
    const int error = errno;
    ::close(fd);
    return error == EWOULDBLOCK ? Outcome::Busy : Outcome::Failed;
#endif
  }

  // Replaces the file's content with `text`; false if it could not be written.
  bool write_text(std::string_view text) noexcept {
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    OVERLAPPED at{};
    if (WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, &at) == FALSE ||
        written != text.size())
      return false;
    return SetEndOfFile(handle_) != FALSE;
#else
    if (fd_ < 0 || ::ftruncate(fd_, 0) != 0) return false;
    std::size_t done = 0;
    while (done < text.size()) {
      const ssize_t n = ::pwrite(fd_, text.data() + done, text.size() - done, static_cast<off_t>(done));
      if (n <= 0) return false;
      done += static_cast<std::size_t>(n);
    }
    return true;
#endif
  }

  // The file's content (up to 4 KiB); empty when it cannot be read.
  [[nodiscard]] std::string read_text() const {
    std::string out(4096, '\0');
#ifdef _WIN32
    if (handle_ == INVALID_HANDLE_VALUE) return {};
    DWORD got = 0;
    OVERLAPPED at{};
    if (ReadFile(handle_, out.data(), static_cast<DWORD>(out.size()), &got, &at) == FALSE) return {};
    out.resize(got);
#else
    if (fd_ < 0) return {};
    const ssize_t n = ::pread(fd_, out.data(), out.size(), 0);
    out.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
#endif
    return out;
  }

  // Closing the descriptor (handle) drops the lock.
  void release() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
#else
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
  }

 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

std::string process_id() {
#ifdef _WIN32
  return std::to_string(GetCurrentProcessId());
#else
  return std::to_string(::getpid());
#endif
}

// What a write uses for one record: the bus (null = none) and the clock
// attached at that moment. Holding it keeps an attachment's destructor waiting.
struct Route {
  std::shared_ptr<SignalBus> bus;
  std::shared_ptr<const Clock> clock;
};

}  // namespace

namespace {
// Defined with the crash handlers below; called from the flusher thread.
void refresh_crash_output(const LogHub::Impl& impl) noexcept;
}  // namespace

struct LogHub::Impl {
  // Given to create(); fixed for the hub's life.
  const Clock* base_clock = nullptr;
  SignalBus* base_bus = nullptr;
  SignalBus::Subscription bus_failures;  // handlers of the create() bus that threw
  bool echo_stderr = false;
  std::atomic<LogLevel> dock_level{LogLevel::Info};  // set_dock_level() may move it at any time
  std::mutex echo_mutex;

  // Attachments. `route_mutex` guards the members below it; it is never held
  // across a publish, a write or a subscription change. A write copies the
  // two refs (pin); an attached ref's deleter marks its id in `pending`
  // released, and the attachment's destructor waits (real time) for that.
  // Declared before the refs: a deleter may run while they are destroyed.
  std::mutex route_mutex;
  std::condition_variable route_cv;
  std::vector<std::pair<std::uint64_t, bool>> pending;  // attachment id, released
  std::atomic<std::uint64_t> next_attachment{0};
  std::shared_ptr<SignalBus> base_bus_ref;        // no-op deleter; may hold null
  std::shared_ptr<const Clock> base_clock_ref;    // no-op deleter
  std::shared_ptr<SignalBus> bus_ref;             // what writes publish on now
  std::shared_ptr<const Clock> clock_ref;         // what writes stamp with now
  std::uint64_t bus_id = 0;                       // 0: create()'s
  std::uint64_t clock_id = 0;
  SignalBus::Subscription attached_failures;      // handlers of the attached bus that threw

  FileLock file_lock;  // on `<dir>/<file_name>.lock`, for the hub's life

  // Session sentinel (spec 3.3): `<dir>/<stem>.session`, locked from
  // begin_session to the destructor. `previous` is what create() found of a
  // session that did not end cleanly; fixed from then on.
  std::mutex session_mutex;
  FileLock session_lock;
  std::filesystem::path session_path;  // empty until the sentinel exists
  std::optional<PreviousSession> previous;

  // Level rules. `epoch` is bumped (under the unique lock) on every change;
  // hub loggers cache (level, epoch) and re-resolve when it moves.
  mutable std::shared_mutex rules_mutex;
  LogLevel default_level = LogLevel::Info;
  std::vector<std::pair<std::string, LogLevel>> rules;
  std::atomic<std::uint64_t> epoch{1};
  std::atomic<bool> session_begun{false};

  // Liveness: loggers hold this Impl and may outlive the LogHub. The
  // destructor clears `alive` and waits for in-flight writes to drain before
  // tearing the back end down; later writes are no-ops.
  std::atomic<bool> alive{true};
  std::atomic<int> in_flight{0};

  std::shared_ptr<spdlog::details::thread_pool> pool;
  std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> file_sink;  // null = no file
  std::filesystem::path file_path;                                   // empty = no file
  std::filesystem::path crash_path;  // <dir>/crash/<stem>-<pid>.txt; empty = no file
  std::shared_ptr<spdlog::async_logger> logger;
  std::shared_ptr<BarrierSink> barrier_sink;
  std::shared_ptr<spdlog::async_logger> barrier;

  std::mutex barrier_mutex;  // orders barrier numbering with enqueueing
  std::uint64_t barriers_posted = 0;

  std::mutex flusher_mutex;
  std::condition_variable flusher_cv;
  bool stopping = false;
  std::thread flusher;

  std::atomic<std::int64_t> last_error_report{std::numeric_limits<std::int64_t>::min()};

  // spdlog error handler: one stderr line, at most once per 10 s.
  void report_spdlog_error(const std::string& what) noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    auto last = last_error_report.load();
    const auto interval =
        std::chrono::duration_cast<std::chrono::milliseconds>(kErrorReportInterval).count();
    if (last != std::numeric_limits<std::int64_t>::min() && now - last < interval) return;
    if (!last_error_report.compare_exchange_strong(last, now)) return;
    try {
      write_stderr("nobion logging error: " + what + '\n');
    } catch (...) {  // NOLINT(bugprone-empty-catch): nowhere left to report that reporting failed
    }
  }

  void report_startup_error(const std::string& message) noexcept {
    try {
      if (base_bus != nullptr) {
        base_bus->publish(Log{LogLevel::Error, "logging", message, base_clock->now()});
      } else {
        std::cerr << "[error] logging: " << message << '\n';
      }
    } catch (...) {  // NOLINT(bugprone-empty-catch): nowhere left to report that reporting failed
    }
  }

  void put_rule(std::string_view pattern, LogLevel level) {
    for (auto& rule : rules) {
      if (rule.first == pattern) {
        rule.second = level;
        return;
      }
    }
    rules.emplace_back(std::string(pattern), level);
  }

  // Most specific matching rule; ties: longer pattern, then the
  // lexicographically greater one. Caller holds rules_mutex.
  LogLevel resolve_locked(std::string_view name) const noexcept {
    const std::pair<std::string, LogLevel>* best = nullptr;
    auto key = [](const std::string& p) {
      return std::make_tuple(log_rule_specificity(p), p.size(), std::string_view(p));
    };
    for (const auto& rule : rules) {
      if (!log_name_matches(rule.first, name)) continue;
      if (best == nullptr || key(rule.first) > key(best->first)) best = &rule;
    }
    return best != nullptr ? best->second : default_level;
  }

  Route pin() {
    std::lock_guard lock(route_mutex);
    return {bus_ref, clock_ref};
  }

  // Deleter of an attached ref: the last write using it is done.
  void release(std::uint64_t id) noexcept {
    try {
      {
        std::lock_guard lock(route_mutex);
        for (auto& entry : pending) {
          if (entry.first == id) entry.second = true;
        }
      }
      route_cv.notify_all();
    } catch (...) {  // NOLINT(bugprone-empty-catch): a mutex that cannot be locked; nothing else to do
    }
  }

  // Puts back create()'s bus or clock if attachment `id` is still the
  // current one, then waits until no write holds what `id` attached.
  void detach(std::uint64_t id) noexcept {
    try {
      std::shared_ptr<SignalBus> old_bus;
      std::shared_ptr<const Clock> old_clock;
      SignalBus::Subscription old_failures;
      {
        std::lock_guard lock(route_mutex);
        if (bus_id == id) {
          old_bus = std::exchange(bus_ref, base_bus_ref);
          old_failures = std::move(attached_failures);
          bus_id = 0;
        }
        if (clock_id == id) {
          old_clock = std::exchange(clock_ref, base_clock_ref);
          clock_id = 0;
        }
      }
      // Outside the lock: the last ref's deleter takes it.
      old_failures.reset();
      old_bus.reset();
      old_clock.reset();

      std::unique_lock lock(route_mutex);
      auto entry = [&] {
        return std::find_if(pending.begin(), pending.end(),
                            [id](const auto& p) { return p.first == id; });
      };
      route_cv.wait(lock, [&] {
        const auto it = entry();
        return it == pending.end() || it->second;
      });
      if (const auto it = entry(); it != pending.end()) pending.erase(it);
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to tell
    }
  }

  // A ref to `target` whose deleter releases attachment `id`, registered in
  // `pending` so detach(id) can wait for it.
  template <class T>
  std::shared_ptr<T> attached_ref(T& target, std::uint64_t id) {
    {
      std::lock_guard lock(route_mutex);
      pending.emplace_back(id, false);
    }
    return std::shared_ptr<T>(&target, [this, id](T*) noexcept { release(id); });
  }

  // Runs entirely on the calling thread apart from the file enqueue, so bus
  // subscribers may log (even at error) without deadlocking the writer.
  void write(LogLevel level, std::string_view name, std::string_view message) noexcept {
    in_flight.fetch_add(1);
    struct Leave {
      std::atomic<int>& n;
      ~Leave() { n.fetch_sub(1); }
    } leave{in_flight};
    if (!alive.load()) return;

    // Declared after `leave`: released before this write counts as done, so
    // the hub (and the deleters' state) is still there when it is.
    Route route;
    try {
      route = pin();
    } catch (...) {
      route.clock = base_clock_ref;  // a copy of a ref never throws
    }
    const Clock& clock = *route.clock;

    // Read here, on the calling thread: the writer thread has its own.
    std::string thread;
    std::string run;
    try {
      thread = detail::thread_label();
      run = std::string(LogContext::run_id());
    } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory; the record goes on without its context
    }

    try {
      // `[executor] run=12345-01A experiment.run: msg`; no `run=` outside a run.
      std::string payload;
      payload.reserve(thread.size() + run.size() + name.size() + message.size() + 12);
      payload.append("[").append(thread).append("] ");
      if (!run.empty()) payload.append("run=").append(run).append(" ");
      payload.append(name).append(": ");
      append_one_line(payload, message);
      // Raw string_view overload: the message is never a format string.
      logger->log(clock.wall_now(), spdlog::source_loc{}, to_spdlog(level),
                  spdlog::string_view_t(payload.data(), payload.size()));
      if (level >= LogLevel::Error) sync_flush();
    } catch (const std::exception& e) {
      report_spdlog_error(e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to say of an exception that has no what()
    }

    if (echo_stderr) {
      try {
        std::lock_guard lock(echo_mutex);
        std::string line;
        line.reserve(name.size() + message.size() + 12);
        line.append("[").append(to_string(level)).append("] ").append(name).append(": ");
        append_one_line(line, message);
        line.append("\n");
        write_stderr(line);
      } catch (...) {  // NOLINT(bugprone-empty-catch): stderr is gone; the file and the bus still have the line
      }
    }

    if (route.bus != nullptr && level >= dock_level.load(std::memory_order_relaxed)) {
      try {
        route.bus->publish(Log{level, std::string(name), std::string(message), clock.now(),
                               std::move(thread), std::move(run)});
      } catch (...) {  // NOLINT(bugprone-empty-catch): a subscriber's failure is not the logger's
      }
    }
  }

  void sync_flush() {
    std::uint64_t target = 0;
    {
      std::lock_guard lock(barrier_mutex);
      target = ++barriers_posted;
      barrier->log(spdlog::level::info, spdlog::string_view_t("barrier"));
    }
    barrier_sink->wait_for(target);
    if (file_sink) file_sink->flush();
  }

  // Terminate route: enqueue `text` at error and flush, waiting at most
  // kCrashFlushTimeout. Skipped on the writer thread, which cannot wait on
  // itself. Never throws.
  void crash_flush(std::string_view text) noexcept {
    in_flight.fetch_add(1);
    struct Leave {
      std::atomic<int>& n;
      ~Leave() { n.fetch_sub(1); }
    } leave{in_flight};
    if (!alive.load() || t_is_log_writer) return;

    try {
      // Same line shape as write(): a thread field, then logger and message.
      std::string payload = "[" + detail::thread_label() + "] nobion: ";
      append_one_line(payload, text);
      // System time, not the hub's clock: a clock's wall_now() may take a
      // mutex (VirtualClock does), and terminate can be raised by the thread
      // that holds it.
      logger->log(std::chrono::system_clock::now(), spdlog::source_loc{}, spdlog::level::err,
                  spdlog::string_view_t(payload.data(), payload.size()));
      if (echo_stderr) {
        std::lock_guard lock(echo_mutex);
        write_stderr("[error] ");
        write_stderr(payload);
        write_stderr("\n");
      }

      const auto deadline = std::chrono::steady_clock::now() + kCrashFlushTimeout;
      std::unique_lock lock(barrier_mutex, std::defer_lock);
      while (!lock.try_lock()) {  // the crashing thread may already hold it
        if (std::chrono::steady_clock::now() >= deadline) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const std::uint64_t target = ++barriers_posted;
      barrier->log(spdlog::level::info, spdlog::string_view_t("barrier"));
      lock.unlock();
      if (!barrier_sink->wait_until(target, deadline)) return;
      if (file_sink) file_sink->flush();
    } catch (...) {  // NOLINT(bugprone-empty-catch): a flush that could not be made; the next one is tried as usual
    }
  }

  void run_flusher() {
    install_signal_stack();
    std::unique_lock lock(flusher_mutex);
    while (!stopping) {
      flusher_cv.wait_for(lock, kPeriodicFlush, [&] { return stopping; });
      if (stopping) break;
      lock.unlock();
      logger->flush();  // asynchronous: queued behind pending records
      refresh_crash_output(*this);  // follow nobion.log across rotation
#ifndef _WIN32
      detail::ModuleMap::refresh();  // a library loaded since the last tick
#endif
      lock.lock();
    }
  }
};

namespace {

// A subscriber of `bus` that throws is otherwise silent. Its first failure is
// written, then the 10th, the 100th...: one that throws at every reading must
// not fill the log, and this record is itself an event on the bus.
SignalBus::Subscription subscribe_failures(SignalBus& bus, const std::shared_ptr<LogHub::Impl>& impl) {
  return bus.subscribe<HandlerFailed>([weak = std::weak_ptr<LogHub::Impl>(impl)](const HandlerFailed& e) {
    std::uint64_t power = 1;
    while (power < e.count && power <= e.count / 10) power *= 10;
    if (e.count != power) return;
    const auto self = weak.lock();
    if (!self) return;
    std::string message = "a handler of " + e.event + " threw: " + e.what;
    if (e.count > 1) message += " (" + std::to_string(e.count) + " times so far)";
    self->write(LogLevel::Error, "bus", message);
  });
}

// ---- Crash handlers (spec 4.5) --------------------------------------------

#ifdef _WIN32
using CrashTarget = HANDLE;
const CrashTarget kNoCrashTarget = INVALID_HANDLE_VALUE;
#else
using CrashTarget = int;
constexpr CrashTarget kNoCrashTarget = -1;
#endif

// Descriptor on the registered hub's nobion.log; read lock-free by the
// signal/exception handler.
std::atomic<CrashTarget> g_crash_target{kNoCrashTarget};
std::atomic<std::terminate_handler> g_previous_terminate{nullptr};

// Normal-context state. Leaked so it survives static destruction.
struct CrashState {
  std::mutex mutex;
  std::weak_ptr<LogHub::Impl> current;  // most recently created hub
  bool installed = false;
#ifdef _WIN32
  std::filesystem::path crash_path;  // what g_crash_text (and g_dump_target) is open on; empty = none
#endif
};
CrashState& crash_state() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): never freed, on purpose: it must outlive every static
  static auto* state = new CrashState;
  return *state;
}

// Opens a file for appending, creating it if need be: the log file when a hub
// is retargeted, the crash file when a crash happens. Called from the signal
// handler (POSIX: open(2) only) and the exception filter.
CrashTarget open_crash_file(const std::filesystem::path::value_type* path) noexcept {
#ifdef _WIN32
  return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
  return ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
#endif
}

CrashTarget open_crash_target(const std::filesystem::path& path) noexcept {
  return open_crash_file(path.c_str());
}

void close_crash_target(CrashTarget target) noexcept {
  if (target == kNoCrashTarget) return;
#ifdef _WIN32
  CloseHandle(target);
#else
  ::close(target);
#endif
}

#ifdef _WIN32

// Windows (spec 3.2): the crash file and the minidump are both opened before
// any crash (at install, when the handlers follow another hub, at
// begin_session), so the filter only calls WriteFile: CreateFileW goes
// through the process heap, which a crash may have broken. Read lock-free by
// the filter and the crash thread.
std::atomic<HANDLE> g_crash_text{INVALID_HANDLE_VALUE};   // <dir>/crash/<stem>-<pid>.txt
std::atomic<HANDLE> g_dump_target{INVALID_HANDLE_VALUE};  // <dir>/crash/<stem>-<pid>.dmp

// Marks `file` to go when its last handle closes (true), or to stay (false).
// A process that does not crash leaves no empty crash file or dump behind;
// the filter (and the terminate line) clear the mark before writing.
void set_delete_on_close(HANDLE file, bool remove) noexcept {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = static_cast<BOOLEAN>(remove ? TRUE : FALSE);
  (void)SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                   static_cast<DWORD>(sizeof(disposition)));
}

// Opens the crash file `crash_path` and the dump beside it (same stem,
// `.dmp`) unless they are the ones open already, and closes the ones they
// replace (removed then, if never written). A file already there (a pid used
// again) is kept: an old crash file is appended to and left unmarked; an old
// dump is left alone and this process goes without one. Caller holds
// CrashState::mutex.
void set_crash_handles(CrashState& state, const std::filesystem::path& crash_path) noexcept {
  try {
    if (crash_path.empty() || crash_path == state.crash_path) return;
    const HANDLE text = CreateFileW(crash_path.c_str(), FILE_APPEND_DATA | DELETE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD text_opened = GetLastError();  // ERROR_ALREADY_EXISTS: it was there before
    if (text != INVALID_HANDLE_VALUE && text_opened != ERROR_ALREADY_EXISTS) set_delete_on_close(text, true);
    close_crash_target(g_crash_text.exchange(text));

    std::filesystem::path dump_path = crash_path;
    dump_path.replace_extension(".dmp");
    const HANDLE dump = CreateFileW(dump_path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) set_delete_on_close(dump, true);
    close_crash_target(g_dump_target.exchange(dump));
    state.crash_path = crash_path;
  } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory; the files opened before stay
  }
}

#else

// The crash file's path, preformatted outside the handler (at install, when a
// hub is retargeted, at begin_session); the handler opens it with open(2)
// only when a crash happens. Two buffers: a new path is written into the one
// the handler is not told about, then named, so the handler never reads one
// half written. Written only under CrashState::mutex.
constexpr std::size_t kCrashPathMax = 4096;
std::array<std::array<char, kCrashPathMax>, 2> g_crash_paths{};
std::atomic<int> g_crash_path_index{-1};  // -1: no crash file

void set_crash_path(const std::filesystem::path& path) noexcept {
  const std::string& text = path.native();  // a reference: nothing allocated
  if (text.empty() || text.size() >= kCrashPathMax) return;
  const int next = g_crash_path_index.load() == 0 ? 1 : 0;
  auto& buffer = g_crash_paths[static_cast<std::size_t>(next)];
  std::copy(text.begin(), text.end(), buffer.begin());
  buffer[text.size()] = '\0';
  g_crash_path_index.store(next);
}

// The crash file, opened now; kNoCrashTarget when there is none.
CrashTarget open_crash_file_now() noexcept {
  const int index = g_crash_path_index.load();
  if (index < 0) return kNoCrashTarget;
  return open_crash_file(g_crash_paths[static_cast<std::size_t>(index)].data());
}

#endif

// The crash file (and on Windows the dump) for `impl`. Caller holds
// CrashState::mutex.
void set_crash_files(CrashState& state, const LogHub::Impl& impl) noexcept {
#ifdef _WIN32
  set_crash_handles(state, impl.crash_path);
#else
  (void)state;
  set_crash_path(impl.crash_path);
#endif
}

// Points the handler at `impl`'s log file and crash file (and, on Windows,
// its dump). Caller holds CrashState::mutex.
//
// POSIX: the handler's descriptor number never changes once set. A new file
// is opened and dup2()ed onto it, which swaps the open file atomically with
// respect to a handler running concurrently, so no descriptor the handler may
// be using is ever closed.
void retarget_crash_output(const LogHub::Impl& impl) noexcept {
  if (impl.file_path.empty()) return;
  set_crash_files(crash_state(), impl);
  const CrashTarget fresh = open_crash_target(impl.file_path);
  if (fresh == kNoCrashTarget) return;
#ifdef _WIN32
  close_crash_target(g_crash_target.exchange(fresh));
#else
  const CrashTarget current = g_crash_target.load();
  if (current == kNoCrashTarget) {
    g_crash_target.store(fresh);
    return;
  }
  ::dup2(fresh, current);  // dup2 clears FD_CLOEXEC on `current`; restore it
  ::fcntl(current, F_SETFD, FD_CLOEXEC);
  close_crash_target(fresh);
#endif
}

// Flusher tick: if `impl` is the hub the handlers write to and its
// nobion.log has been replaced (rotation), retarget onto the live file.
void refresh_crash_output(const LogHub::Impl& impl) noexcept {
#ifdef _WIN32
  (void)impl;  // the handle is only (re)opened when a hub is created
#else
  if (impl.file_path.empty()) return;
  auto& state = crash_state();
  std::unique_lock lock(state.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !state.installed) return;
  if (state.current.lock().get() != &impl) return;
  const CrashTarget current = g_crash_target.load();
  struct stat live {};
  struct stat open_file {};
  if (::stat(impl.file_path.c_str(), &live) != 0) return;  // mid-rotation; next tick
  if (current != kNoCrashTarget && ::fstat(current, &open_file) == 0 &&
      live.st_dev == open_file.st_dev && live.st_ino == open_file.st_ino)
    return;
  retarget_crash_output(impl);
#endif
}

// begin_session: the crash path (and on Windows the dump) is set again, for
// the hub the handlers follow.
void reset_crash_path(const LogHub::Impl& impl) noexcept {
  try {
    auto& state = crash_state();
    std::lock_guard lock(state.mutex);
    if (!state.installed || state.current.lock().get() != &impl) return;
    set_crash_files(state, impl);
  } catch (...) {  // NOLINT(bugprone-empty-catch): a mutex that cannot be locked; the path set at install stays
  }
}

// Hub start: of the crashes in `dir` (a `<stem>.txt` and its `<stem>.dmp`
// are one), the newest kCrashesKept stay. Errors leave files in place.
void prune_crashes(const std::filesystem::path& dir) noexcept {
  try {
    struct Crash {
      std::string stem;
      std::filesystem::file_time_type newest;
      std::vector<std::filesystem::path> files;
    };
    std::vector<Crash> crashes;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      const auto& path = it->path();
      const auto ext = path.extension();
      if (ext != ".txt" && ext != ".dmp") continue;
      if (!it->is_regular_file(ec)) {
        ec.clear();  // a live process's file on Windows is held open: not ours to look at
        continue;
      }
      const auto time = it->last_write_time(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      const std::string stem = path.stem().string();
      auto crash = std::find_if(crashes.begin(), crashes.end(), [&](const Crash& c) { return c.stem == stem; });
      if (crash == crashes.end()) {
        crashes.push_back({stem, time, {}});
        crash = std::prev(crashes.end());
      }
      crash->newest = std::max(crash->newest, time);
      crash->files.push_back(path);
    }
    if (crashes.size() <= kCrashesKept) return;
    std::sort(crashes.begin(), crashes.end(), [](const Crash& a, const Crash& b) { return a.newest > b.newest; });
    for (std::size_t i = kCrashesKept; i < crashes.size(); ++i) {
      for (const auto& file : crashes[i].files) std::filesystem::remove(file, ec);
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): pruning is housekeeping; a failure leaves old crashes
  }
}

// Async-signal-safe write of a whole buffer; errors are ignored.
void write_all(CrashTarget target, const char* data, std::size_t size) noexcept {
  if (target == kNoCrashTarget) return;
#ifdef _WIN32
  DWORD written = 0;
  WriteFile(target, data, static_cast<DWORD>(size), &written, nullptr);
#else
  while (size > 0) {
    const ssize_t n = ::write(target, data, size);
    if (n <= 0) return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
#endif
}

// Appends the decimal (or, with base 16, hex) digits of `value` to buf at pos.
std::size_t put_unsigned(char* buf, std::size_t pos, std::uint64_t value, unsigned base) noexcept {
  char digits[24];
  std::size_t n = 0;
  do {
    digits[n++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value != 0 && n < sizeof digits);
  while (n > 0) buf[pos++] = digits[--n];
  return pos;
}

std::string terminate_text() {
  const auto active = std::current_exception();
  if (!active) return "terminate called without an active exception";
  try {
    // cppcheck-suppress missingReturn ; std::rethrow_exception does not return
    std::rethrow_exception(active);
  } catch (const std::exception& e) {
    return std::string("terminate called after throwing: ") + e.what();
  } catch (...) {
    return "terminate called after throwing a non-std::exception";
  }
}

// The terminate text as one line of the crash file. Not a signal context
// (terminate runs on the throwing thread as a normal call), so the line is
// built here; the file is written as the handler writes it (POSIX: open(2)
// and write(2); Windows: WriteFile on the handle opened in advance), so the
// SIGABRT block that abort() adds next follows it. (MSVC's abort() ends the
// process without the exception filter: on Windows this line is all the
// crash file has.)
void write_terminate_line(std::string_view text) noexcept {
#ifdef _WIN32
  const CrashTarget file = g_crash_text.load();
  if (file == kNoCrashTarget) return;
  set_delete_on_close(file, false);
#else
  const CrashTarget file = open_crash_file_now();
  if (file == kNoCrashTarget) return;
#endif
  try {
    std::string line;
    append_one_line(line, text);
    line.push_back('\n');
    write_all(file, line.data(), line.size());
  } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory; the SIGABRT block still follows
  }
#ifndef _WIN32
  close_crash_target(file);
#endif
}

[[noreturn]] void on_terminate() noexcept {
  std::string text;
  try {
    text = terminate_text();
  } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory; the fallback below is static
  }
  const std::string_view line = text.empty() ? std::string_view("terminate called") : std::string_view(text);
  write_terminate_line(line);

  std::shared_ptr<LogHub::Impl> hub;
  {
    // Bounded: terminate may fire on a thread that already holds the mutex.
    auto& state = crash_state();
    std::unique_lock lock(state.mutex, std::defer_lock);
    for (int i = 0; i < 100 && !lock.try_lock(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (lock.owns_lock()) hub = state.current.lock();
  }
  if (hub) {
    try {
      hub->crash_flush(line);
    } catch (...) {  // NOLINT(bugprone-empty-catch): terminating already
    }
  }
  hub.reset();
  if (const auto previous = g_previous_terminate.load()) previous();
  std::abort();
}

// "build <build_id>\n", preformatted at install.
std::array<char, 160> g_build_line{};
std::size_t g_build_line_size = 0;

void preformat_build_line() noexcept {
  try {
    const std::string build = "build " + build_id() + "\n";
    const std::size_t n = std::min(build.size(), g_build_line.size());
    std::copy_n(build.begin(), n, g_build_line.begin());
    g_build_line_size = n;
  } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory at install; the crash file goes without its build line
  }
}

// Set by the first thread into the handler (filter). Only that one writes.
std::atomic<bool> g_in_crash{false};

constexpr std::size_t kMaxTagBytes = 128;

// stderr, the log file, the crash file; kNoCrashTarget for one that is not there.
using CrashTargets = std::array<CrashTarget, 3>;

void write_to_all(const CrashTargets& targets, const char* data, std::size_t size) noexcept {
  for (const CrashTarget target : targets) write_all(target, data, size);
}

// "thread <tag>\n" from the crashing thread's LogContext tag; "?" if none.
void write_thread_line(const CrashTargets& targets) noexcept {
  std::array<char, kMaxTagBytes + 16> line{};
  std::size_t pos = 0;
  for (const char c : std::string_view("thread ")) line[pos++] = c;
  const std::string* tag = detail::thread_tag_pointer();
  if (tag != nullptr && !tag->empty()) {
    const char* data = tag->data();
    const std::size_t n = std::min(tag->size(), kMaxTagBytes);
    for (std::size_t i = 0; i < n; ++i) line[pos++] = data[i] == '\n' ? ' ' : data[i];
  } else {
    line[pos++] = '?';
  }
  line[pos++] = '\n';
  write_to_all(targets, line.data(), pos);
}

#ifdef _WIN32

// Windows (spec 3.2): the filter writes the text lines on the faulting
// thread, then hands the EXCEPTION_POINTERS to the crash thread, which writes
// the minidump: a stack overflow leaves the faulting thread too little stack
// for dbghelp. Events and thread are made at install, the crash file and
// dump handles then too (set_crash_handles). The filter's stack after an
// overflow is the 64 KiB guarantee install_signal_stack() gives each thread
// we own.
constexpr DWORD kDumpWaitMs = 10000;
// A second faulting thread waits this long: past the first's dump wait.
constexpr DWORD kSecondCrashWaitMs = kDumpWaitMs + 5000;
std::atomic<HANDLE> g_dump_request{nullptr};  // auto-reset; the filter sets it
std::atomic<HANDLE> g_dump_done{nullptr};     // manual-reset; the crash thread sets it
std::atomic<EXCEPTION_POINTERS*> g_dump_pointers{nullptr};
std::atomic<DWORD> g_dump_thread_id{0};

DWORD WINAPI dump_thread_main(LPVOID /*unused*/) {
  const HANDLE request = g_dump_request.load();
  if (request == nullptr || WaitForSingleObject(request, INFINITE) != WAIT_OBJECT_0) return 0;
  const HANDLE dump = g_dump_target.load();
  if (dump != INVALID_HANDLE_VALUE) {
    set_delete_on_close(dump, false);
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = g_dump_thread_id.load();
    exception.ExceptionPointers = g_dump_pointers.load();
    exception.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
    (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type,
                            exception.ExceptionPointers != nullptr ? &exception : nullptr, nullptr, nullptr);
  }
  const HANDLE done = g_dump_done.load();
  if (done != nullptr) (void)SetEvent(done);
  return 0;
}

// Starts the crash thread. Without it (no event, no thread) the filter
// writes its text and does not wait.
void start_dump_thread() noexcept {
  const HANDLE request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (request != nullptr && done != nullptr) {
    g_dump_request.store(request);
    g_dump_done.store(done);
    const HANDLE thread = CreateThread(nullptr, 0, &dump_thread_main, nullptr, 0, nullptr);
    if (thread != nullptr) {
      (void)CloseHandle(thread);  // the thread runs on; nothing joins it
      return;
    }
    g_dump_request.store(nullptr);
    g_dump_done.store(nullptr);
  }
  if (request != nullptr) (void)CloseHandle(request);
  if (done != nullptr) (void)CloseHandle(done);
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) {
  // A second thread faulting meanwhile waits until the first has finished
  // its dump (and ended the process), then lets its own exception go on.
  if (g_in_crash.exchange(true)) {
    Sleep(kSecondCrashWaitMs);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Only WriteFile from here on the faulting thread: the handles were opened
  // in advance, as nothing that may touch the heap is safe now.
  const CrashTarget crash_file = g_crash_text.load();
  if (crash_file != kNoCrashTarget) set_delete_on_close(crash_file, false);
  const CrashTargets targets{GetStdHandle(STD_ERROR_HANDLE), g_crash_target.load(), crash_file};

  std::array<char, 64> line{};
  std::size_t pos = 0;
  for (const char c : std::string_view("fatal exception 0x")) line[pos++] = c;
  const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                         ? static_cast<DWORD>(info->ExceptionRecord->ExceptionCode)
                         : DWORD{0};
  pos = put_unsigned(line.data(), pos, static_cast<std::uint64_t>(code), 16);
  line[pos++] = '\n';
  write_to_all(targets, line.data(), pos);
  write_to_all(targets, g_build_line.data(), g_build_line_size);
  write_thread_line(targets);

  pos = 0;
  for (const char c : std::string_view("thread id ")) line[pos++] = c;
  pos = put_unsigned(line.data(), pos, static_cast<std::uint64_t>(GetCurrentThreadId()), 10);
  line[pos++] = '\n';
  write_to_all(targets, line.data(), pos);

  // More preformatted spans go here (the flight recorder ring, spec 5.4).

  const HANDLE request = g_dump_request.load();
  const HANDLE done = g_dump_done.load();
  if (request != nullptr && done != nullptr) {
    g_dump_thread_id.store(GetCurrentThreadId());
    g_dump_pointers.store(info);
    if (SetEvent(request) != FALSE) (void)WaitForSingleObject(done, kDumpWaitMs);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_fatal_handlers() noexcept {
  preformat_build_line();
  start_dump_thread();
  (void)SetUnhandledExceptionFilter(&on_unhandled_exception);
}

#else

// Everything the handler writes is formatted here or on its own stack from
// bytes made outside the signal; the handler calls only write(2), open(2),
// backtrace(), alarm(2), sigaction(2) and raise(3) (spec 3.2).

// "fatal signal N\n", preformatted at install time so the handler only
// copies bytes.
struct SignalLine {
  int signal = 0;
  std::array<char, 32> text{};
  std::size_t size = 0;
};
constexpr std::array kFatalSignals{SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
std::array<SignalLine, kFatalSignals.size()> g_signal_lines;

// The first thread into the handler owns this frame array.
constexpr int kMaxFrames = 64;
std::array<void*, kMaxFrames> g_frames{};

constexpr unsigned kHandlerAlarmSeconds = 5;

// The raw backtrace, one `0x<hex>` a line. No symbol names: tools/symbolize.py
// turns these and the module map into functions and lines.
void write_frames(const CrashTargets& targets) noexcept {
  const int n = ::backtrace(g_frames.data(), kMaxFrames);
  for (int i = 0; i < n; ++i) {
    std::array<char, 24> line{};
    std::size_t pos = 0;
    line[pos++] = '0';
    line[pos++] = 'x';
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the address is what is written
    const auto address = reinterpret_cast<std::uintptr_t>(g_frames[static_cast<std::size_t>(i)]);
    pos = put_unsigned(line.data(), pos, address, 16);
    line[pos++] = '\n';
    write_to_all(targets, line.data(), pos);
  }
}

// The interrupted instruction, from the signal's machine context; 0 where
// this file does not know the context's layout.
std::uintptr_t faulting_pc(const void* context) noexcept {
  if (context == nullptr) return 0;
  const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
  return static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
#elif defined(__APPLE__) && defined(__x86_64__)
  return static_cast<std::uintptr_t>(uc->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
  return static_cast<std::uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
  return static_cast<std::uintptr_t>(uc->uc_mcontext.pc);
#else
  (void)uc;
  return 0;
#endif
}

// `pc 0x<hex>`: the faulting instruction, exact, where every backtrace frame
// is a return address. glibc's backtrace() has it as the frame after the
// signal trampoline; macOS's goes from the trampoline to the faulting
// function's caller and leaves it out. tools/symbolize.py names it as it is
// and looks every other frame up at address - 1.
void write_pc_line(const CrashTargets& targets, std::uintptr_t pc) noexcept {
  if (pc == 0) return;
  std::array<char, 28> line{};
  std::size_t pos = 0;
  for (const char c : std::string_view("pc 0x")) line[pos++] = c;
  pos = put_unsigned(line.data(), pos, pc, 16);
  line[pos++] = '\n';
  write_to_all(targets, line.data(), pos);
}

void on_fatal_signal(int sig, siginfo_t* /*info*/, void* context) {
  // First act: a handler stuck for any reason still ends the process.
  ::alarm(kHandlerAlarmSeconds);
  // A second thread crashing meanwhile waits for the first to end the
  // process (or for the alarm).
  if (g_in_crash.exchange(true)) {
    while (g_in_crash.load()) {
      // the first crash ends the process
    }
  }

  const int crash_fd = open_crash_file_now();
  const CrashTargets targets{STDERR_FILENO, g_crash_target.load(), crash_fd};

  for (const auto& line : g_signal_lines) {
    if (line.signal != sig) continue;
    write_to_all(targets, line.text.data(), line.size);
    break;
  }
  write_to_all(targets, g_build_line.data(), g_build_line_size);
  write_thread_line(targets);
  write_pc_line(targets, faulting_pc(context));
  write_frames(targets);
  // The module map is long (hundreds of lines in the app): the crash file
  // has it, stderr only when there is no crash file.
  detail::ModuleMap::write_to(crash_fd >= 0 ? crash_fd : STDERR_FILENO);

  // More preformatted spans go here, to crash_fd with write(2) only (the
  // flight recorder ring, spec 5.4).

  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  ::sigaction(sig, &default_action, nullptr);
  ::raise(sig);  // delivered with the default action once this returns
}

void install_fatal_handlers() noexcept {
  static constexpr std::string_view kPrefix = "fatal signal ";
  for (std::size_t i = 0; i < kFatalSignals.size(); ++i) {
    auto& line = g_signal_lines[i];
    line.signal = kFatalSignals[i];
    std::size_t pos = 0;
    for (const char c : kPrefix) line.text[pos++] = c;
    pos = put_unsigned(line.text.data(), pos, static_cast<std::uint64_t>(line.signal), 10);
    line.text[pos++] = '\n';
    line.size = pos;
  }
  preformat_build_line();
  // backtrace() loads its unwinder on first use (glibc dlopens libgcc_s):
  // done here, never first in a signal.
  std::array<void*, 4> warm{};
  (void)::backtrace(warm.data(), static_cast<int>(warm.size()));
  detail::ModuleMap::install();

  for (const int sig : kFatalSignals) {
    struct sigaction action {};
    action.sa_sigaction = &on_fatal_signal;
    sigemptyset(&action.sa_mask);
    // A second fault of another kind on the crashing thread is not handled
    // again: the kernel ends the process with it.
    for (const int other : kFatalSignals) sigaddset(&action.sa_mask, other);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(sig, &action, nullptr);
  }
}

#endif

}  // namespace

void LogHub::install_crash_handlers() {
  auto& state = crash_state();
  std::lock_guard lock(state.mutex);
  if (state.installed) return;
  state.installed = true;
  install_signal_stack();  // the calling (main) thread's (Windows: its stack guarantee)
  detail::ensure_thread_tag_key();  // the handler reads the thread tag from it
  install_fatal_handlers();  // before the retarget: it preformats what the handler writes
  if (auto hub = state.current.lock()) retarget_crash_output(*hub);
  g_previous_terminate.store(std::set_terminate(&on_terminate));
}

namespace {

std::string utc_stamp(std::chrono::system_clock::time_point at) {
  const std::time_t t = std::chrono::system_clock::to_time_t(at);
  std::tm parts{};
#ifdef _WIN32
  if (gmtime_s(&parts, &t) != 0) return "unknown";
#else
  if (gmtime_r(&t, &parts) == nullptr) return "unknown";
#endif
  std::array<char, 32> buf{};
  const std::size_t n = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return std::string(buf.data(), n);
}

// The value of line `key=...` in a sentinel's text; empty if there is none.
std::string sentinel_field(const std::string& text, std::string_view key) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string_view line(text.data() + pos, end - pos);
    if (line.size() > key.size() && line.starts_with(key) && line[key.size()] == '=')
      return std::string(line.substr(key.size() + 1));
    pos = end + 1;
  }
  return {};
}

// A crash file this much older than its session's sentinel is still the
// session's: file times are coarse on some file systems (FAT: 2 s).
constexpr auto kCrashFileSlack = std::chrono::seconds(5);

// The program a sentinel is of: its stem, less the `-<pid>` a second hub on
// the folder adds (`elctl-4242.session` with pid 4242 is elctl's). The crash
// file of that process is `crash/<program>-<pid>.txt`.
std::string sentinel_program(const std::filesystem::path& sentinel, const std::string& pid) {
  std::string stem = sentinel.stem().string();
  const std::string suffix = "-" + pid;
  if (!pid.empty() && stem.size() > suffix.size() && stem.ends_with(suffix))
    stem.resize(stem.size() - suffix.size());
  return stem;
}

// Hub start: every `*.session` in the log dir whose lock can be taken belongs
// to a process that is gone without a clean end (a live one holds its lock; a
// reused pid does not matter). Each is logged at error and removed; the
// latest is kept for previous_session(). A sentinel that cannot be opened or
// locked is skipped, never an error: on Windows a live process's files can
// refuse another process.
void find_unclean_sessions(LogHub::Impl& impl) noexcept {
  try {
    const std::filesystem::path dir = impl.file_path.parent_path();
    std::vector<std::filesystem::path> sentinels;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->path().extension() != ".session") continue;
      if (!it->is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      sentinels.push_back(it->path());
    }
    std::sort(sentinels.begin(), sentinels.end());
    for (const auto& path : sentinels) {
      std::string text;
      // When the session began, on the file system's clock (what the crash
      // file's time is compared with): `started=` is the hub clock's wall
      // time, simulated in a simulated session.
      std::optional<std::filesystem::file_time_type> made;
      {
        FileLock probe;
        if (probe.acquire(path, false) != FileLock::Outcome::Held) continue;
        text = probe.read_text();
        std::error_code time_ec;
        const auto when = std::filesystem::last_write_time(path, time_ec);
        if (!time_ec) made = when;
      }  // closed (and so unlocked) before it is removed
      std::error_code remove_ec;
      std::filesystem::remove(path, remove_ec);
      const std::string build = sentinel_field(text, "build");
      const std::string started = sentinel_field(text, "started");
      const std::string pid = sentinel_field(text, "pid");
      if (build.empty() && started.empty()) continue;  // never filled in: nothing to say
      PreviousSession session;
      session.program = sentinel_program(path, pid);
      session.build_id = build;
      session.started = started;
      if (!pid.empty() && !session.program.empty() && made) {
        std::filesystem::path crash = dir / "crash" / (session.program + "-" + pid + ".txt");
        std::error_code crash_ec;
        const auto written = std::filesystem::last_write_time(crash, crash_ec);
        if (!crash_ec && std::filesystem::is_regular_file(crash, crash_ec) &&
            written >= *made - kCrashFileSlack)
          session.crash_file = std::move(crash);
      }
      std::string message = "previous ";
      message.append(session.program).append(" session ").append(build).append(" started ").append(started)
          .append(" ended without a clean exit");
      if (session.crash_file) message += "; crash file " + session.crash_file->string();
      impl.write(LogLevel::Error, "nobion", message);
      if (!impl.previous || impl.previous->started <= session.started) impl.previous = std::move(session);
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch): a look at old files; failing it must not stop logging
  }
}

}  // namespace

Result<std::shared_ptr<LogHub>> LogHub::create(const config::LoggingConfig& config,
                                               const Clock& clock, SignalBus* bus) {
  auto impl = std::make_shared<Impl>();
  impl->base_clock = &clock;
  impl->base_bus = bus;
  impl->base_clock_ref = std::shared_ptr<const Clock>(&clock, [](const Clock*) noexcept {});
  impl->base_bus_ref = std::shared_ptr<SignalBus>(bus, [](SignalBus*) noexcept {});
  impl->clock_ref = impl->base_clock_ref;
  impl->bus_ref = impl->base_bus_ref;
  if (bus != nullptr) impl->bus_failures = subscribe_failures(*bus, impl);
  impl->echo_stderr = config.echo_stderr;
  impl->dock_level.store(config.dock_level);
  impl->default_level = config.default_level;
  for (const auto& [pattern, level] : config.levels) impl->put_rule(pattern, level);

  std::string lock_notice;  // written once the back end runs
  LogLevel lock_notice_level = LogLevel::Info;
  if (config.dir && !config.dir->empty()) {
    const std::filesystem::path& dir = *config.dir;
    std::filesystem::path file = dir / config.file_name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
      std::filesystem::path lock_path = file;
      lock_path += ".lock";
      switch (impl->file_lock.acquire(lock_path)) {
        case FileLock::Outcome::Held: break;
        case FileLock::Outcome::Busy: {
          const std::filesystem::path name(config.file_name);
          const std::filesystem::path taken = file;
          file = dir / (name.stem().string() + "-" + process_id() + name.extension().string());
          lock_notice = taken.string() + " is in use by another hub; this one logs to " + file.string();
          break;
        }
        case FileLock::Outcome::Failed:
          lock_notice_level = LogLevel::Warn;
          lock_notice = "cannot lock " + lock_path.string() + "; logging to " + file.string() +
                        " without a lock";
          break;
      }
    }
    if (ec) {
      impl->report_startup_error("cannot create log directory " + dir.string() + ": " +
                                 ec.message() + "; file logging disabled");
    } else {
      // Total files kept = max_files: spdlog counts rotated files only.
      const auto files = static_cast<std::size_t>(std::max<std::int64_t>(config.max_files, 1));
      const auto size_mb = static_cast<std::size_t>(std::max<std::int64_t>(config.max_size_mb, 1));
      try {
        impl->file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            file.string(), size_mb * 1024 * 1024,
            std::min(files - 1, kSpdlogMaxRotated));
        impl->file_sink->set_formatter(make_file_formatter());
        impl->file_path = std::move(file);
        // Crash files: `<stem>-<pid>.txt` in `<dir>/crash` (spec 3.3); the
        // pid is in the name already, so the configured stem, not the
        // `-<pid>` one a second hub logs to.
        const std::filesystem::path crash_dir = dir / "crash";
        std::error_code crash_ec;
        std::filesystem::create_directories(crash_dir, crash_ec);
        if (!crash_ec) {
          prune_crashes(crash_dir);
          impl->crash_path = crash_dir / (std::filesystem::path(config.file_name).stem().string() + "-" +
                                          process_id() + ".txt");
        }
      } catch (const std::exception& e) {
        impl->file_sink.reset();
        impl->report_startup_error("cannot open log file in " + dir.string() + ": " +
                                   e.what() + "; file logging disabled");
      }
    }
  }

  try {
    impl->pool = std::make_shared<spdlog::details::thread_pool>(
        kQueueSize, kWriterThreads, [] {
          install_signal_stack();
          t_is_log_writer = true;
        });
    std::vector<spdlog::sink_ptr> sinks;
    if (impl->file_sink) sinks.push_back(impl->file_sink);
    impl->logger = std::make_shared<spdlog::async_logger>(
        "nobion", sinks.begin(), sinks.end(), impl->pool, spdlog::async_overflow_policy::block);
    impl->logger->set_level(spdlog::level::trace);
    // Redundant with LogHub::write's synchronous flush, but keeps the writer
    // flushing error records even if a caller bypasses write().
    impl->logger->flush_on(spdlog::level::err);

    impl->barrier_sink = std::make_shared<BarrierSink>();
    impl->barrier = std::make_shared<spdlog::async_logger>(
        "nobion.barrier", impl->barrier_sink, impl->pool, spdlog::async_overflow_policy::block);
    impl->barrier->set_level(spdlog::level::trace);

    Impl* raw = impl.get();
    auto handler = [raw](const std::string& what) { raw->report_spdlog_error(what); };
    impl->logger->set_error_handler(handler);
    impl->barrier->set_error_handler(handler);

    impl->flusher = std::thread([raw] { raw->run_flusher(); });
  } catch (const std::exception& e) {
    return fail(ErrorKind::Io, std::string("cannot start logging: ") + e.what());
  }

  {
    auto& state = crash_state();
    std::lock_guard lock(state.mutex);
    state.current = impl;
    if (state.installed) retarget_crash_output(*impl);
  }
  if (!lock_notice.empty()) impl->write(lock_notice_level, "logging", lock_notice);
  if (!impl->file_path.empty()) find_unclean_sessions(*impl);
  return std::shared_ptr<LogHub>(new LogHub(std::move(impl)));
}

LogHub::LogHub(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

LogHub::~LogHub() {
  if (impl_->session_begun.load()) impl_->write(LogLevel::Info, "nobion", "session end");
  impl_->bus_failures.reset();
  // Hub loggers may still hold impl_: stop accepting writes and let the ones
  // in progress finish before the back end goes away.
  impl_->alive.store(false);
  while (impl_->in_flight.load() != 0) std::this_thread::yield();
  {
    std::lock_guard lock(impl_->flusher_mutex);
    impl_->stopping = true;
  }
  impl_->flusher_cv.notify_all();
  if (impl_->flusher.joinable()) impl_->flusher.join();
  try {
    impl_->sync_flush();
  } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to tell
  }
  // The clean end: the sentinel goes, then its lock.
  if (!impl_->session_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(impl_->session_path, ec);
  }
  impl_->session_lock.release();
  // Loggers first; the pool's destructor drains whatever is still queued and
  // joins the writer thread.
  impl_->logger.reset();
  impl_->barrier.reset();
  impl_->pool.reset();
  impl_->file_sink.reset();
  impl_->barrier_sink.reset();
  impl_->file_lock.release();
}

namespace {

const char* session_level_name(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "info";
}

const char* session_os_name(HostOs os) noexcept {
  switch (os) {
    case HostOs::Mac: return "macos";
    case HostOs::Linux: return "linux";
    case HostOs::Windows: return "windows";
  }
  return "linux";
}

void append_field(std::string& out, std::string_view key, std::string_view value) {
  out += ' ';
  out += key;
  out += '=';
  if (value.find(' ') == std::string_view::npos) {
    out += value;
  } else {
    out += '"';
    out += value;
    out += '"';
  }
}

}  // namespace

namespace {

// `<dir>/<stem>.session` with pid, build and start time, locked for the rest
// of the process. Made under another name and renamed into place once locked
// and filled in: a start that looks for stale sentinels never sees a
// half-made one (the lock follows the file through the rename).
void make_sentinel(LogHub::Impl& impl) noexcept {
  try {
    std::lock_guard lock(impl.session_mutex);
    if (impl.file_path.empty() || !impl.session_path.empty()) return;
    std::filesystem::path final_path = impl.file_path;
    final_path.replace_extension(".session");
    std::filesystem::path building = final_path;
    building += ".new";
    if (impl.session_lock.acquire(building) != FileLock::Outcome::Held) return;
    const std::string text = "pid=" + process_id() + "\nbuild=" + build_id() +
                             "\nstarted=" + utc_stamp(impl.pin().clock->wall_now()) + "\n";
    std::error_code ec;
    bool placed = impl.session_lock.write_text(text);
    if (placed) {
      std::filesystem::rename(building, final_path, ec);
      placed = !ec;
    }
    if (!placed) {
      impl.session_lock.release();
      std::filesystem::remove(building, ec);
      return;
    }
    impl.session_path = std::move(final_path);
  } catch (...) {  // NOLINT(bugprone-empty-catch): no sentinel is a missing report, not a failure
  }
}

}  // namespace

void LogHub::begin_session(const SessionInfo& info) {
  std::string line = "session start";
  append_field(line, "app", info.app);
  append_field(line, "version", info.version);
  append_field(line, "build", build_id());
  append_field(line, "os", session_os_name(host_os()));
  append_field(line, "install", info.install_root ? info.install_root->generic_string() : "none");
  append_field(line, "profiles", info.profiles.empty() ? "none" : info.profiles);
  append_field(line, "sim", info.simulated ? "yes" : "no");
  append_field(line, "clock", info.clock);
  std::string levels;
  {
    std::shared_lock lock(impl_->rules_mutex);
    append_field(line, "default_level", session_level_name(impl_->default_level));
    for (const auto& [pattern, level] : impl_->rules) {
      if (!levels.empty()) levels += ',';
      levels += pattern;
      levels += ':';
      levels += session_level_name(level);
    }
  }
  append_field(line, "dock_level", session_level_name(impl_->dock_level.load()));
  append_field(line, "levels", levels.empty() ? "none" : levels);
  impl_->session_begun.store(true);
  impl_->write(LogLevel::Info, "nobion", line);
  reset_crash_path(*impl_);
  make_sentinel(*impl_);
}

std::optional<PreviousSession> LogHub::previous_session() const { return impl_->previous; }

void LogHub::flush() {
  try {
    impl_->sync_flush();
  } catch (const std::exception& e) {
    impl_->report_spdlog_error(e.what());
  } catch (...) {  // NOLINT(bugprone-empty-catch): nothing to say of an exception that has no what()
  }
}

void LogHub::write(LogLevel level, std::string_view logger, std::string_view message) {
  impl_->write(level, logger, message);
}

Logger LogHub::logger(std::string name) { return Logger(std::move(name), impl_); }

void LogHub::set_dock_level(LogLevel level) noexcept { impl_->dock_level.store(level); }

LogLevel LogHub::dock_level() const noexcept { return impl_->dock_level.load(); }

SignalBus* LogHub::bus() const noexcept {
  try {
    std::lock_guard lock(impl_->route_mutex);
    return impl_->bus_ref.get();
  } catch (...) {
    return impl_->base_bus;
  }
}

std::filesystem::path LogHub::file_path() const { return impl_->file_path; }

std::filesystem::path LogHub::crash_file_path() const { return impl_->crash_path; }

LogHub::BusAttachment LogHub::attach_bus(SignalBus& bus) {
  const std::uint64_t id = impl_->next_attachment.fetch_add(1) + 1;
  SignalBus::Subscription failures = subscribe_failures(bus, impl_);
  // Before `ref`: on unwind, `ref` is released first and the wait returns.
  BusAttachment attachment(impl_, id);
  std::shared_ptr<SignalBus> ref = impl_->attached_ref(bus, id);
  {
    std::lock_guard lock(impl_->route_mutex);
    std::swap(impl_->bus_ref, ref);
    std::swap(impl_->attached_failures, failures);
    impl_->bus_id = id;
  }
  // `ref` and `failures` now hold the replaced bus's; released outside the lock.
  return attachment;
}

LogHub::ClockAttachment LogHub::attach_clock(const Clock& clock) {
  const std::uint64_t id = impl_->next_attachment.fetch_add(1) + 1;
  ClockAttachment attachment(impl_, id);
  std::shared_ptr<const Clock> ref = impl_->attached_ref(clock, id);
  {
    std::lock_guard lock(impl_->route_mutex);
    std::swap(impl_->clock_ref, ref);
    impl_->clock_id = id;
  }
  return attachment;
}

LogHub::Attachment::Attachment(std::shared_ptr<Impl> impl, std::uint64_t id) noexcept
    : impl_(std::move(impl)), id_(id) {}

LogHub::Attachment::~Attachment() { detach(); }

LogHub::Attachment::Attachment(Attachment&& other) noexcept
    : impl_(std::move(other.impl_)), id_(std::exchange(other.id_, 0)) {}

LogHub::Attachment& LogHub::Attachment::operator=(Attachment&& other) noexcept {
  if (this != &other) {
    detach();
    impl_ = std::move(other.impl_);
    id_ = std::exchange(other.id_, 0);
  }
  return *this;
}

void LogHub::Attachment::detach() noexcept {
  if (impl_) impl_->detach(id_);
  impl_.reset();
  id_ = 0;
}

void LogHub::set_level(std::string_view pattern, LogLevel level) {
  std::unique_lock lock(impl_->rules_mutex);
  impl_->put_rule(pattern, level);
  impl_->epoch.fetch_add(1);
}

void LogHub::set_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>& levels) {
  std::unique_lock lock(impl_->rules_mutex);
  impl_->default_level = default_level;
  impl_->rules.clear();
  for (const auto& [pattern, level] : levels) impl_->put_rule(pattern, level);
  impl_->epoch.fetch_add(1);
}

namespace detail {

std::uint64_t hub_rule_epoch(const LogHub::Impl& hub) noexcept { return hub.epoch.load(); }

ResolvedLevel hub_resolve_level(const LogHub::Impl& hub, std::string_view name) noexcept {
  try {
    std::shared_lock lock(hub.rules_mutex);
    return {hub.resolve_locked(name), hub.epoch.load()};
  } catch (...) {
    // Locking failed (system_error); fall back without caching a new epoch.
    return {LogLevel::Error, 0};
  }
}

std::string thread_label() {
  const std::string_view tag = LogContext::thread_tag();
  if (!tag.empty()) return std::string(tag);
  return "t" + std::to_string(LogContext::thread_number());
}

void hub_write(LogHub::Impl& hub, LogLevel level, std::string_view logger,
               std::string_view message) noexcept {
  hub.write(level, logger, message);
}

}  // namespace detail

}  // namespace nobion
