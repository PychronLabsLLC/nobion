#pragma once

// Process-wide logging back end: owns the rotating file sink and its writer
// thread. No spdlog type appears here; everything lives behind Impl.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/error.hpp"
#include "nobion/core/events.hpp"

namespace nobion {

class Logger;
class SignalBus;

// What a process says about itself in its `session start` record.
struct SessionInfo {
  std::string app;                                    // "nobion", "elctl"
  std::string version;                                // nobion::setup::version()
  std::optional<std::filesystem::path> install_root;
  std::string profiles;                               // "" when none
  bool simulated = false;
  std::string clock;                                  // "steady", "virtual x10", "virtual max"
};

// A session of an earlier process on the same log folder that did not end
// cleanly (found by LogHub::create; spec 2026-11-06-field-diagnostics, 3.3).
struct PreviousSession {
  std::string program;                                // the sentinel's stem without a `-<pid>` (`elctl`)
  std::string build_id;
  std::string started;                                // UTC, `2026-11-06T14:03:22Z`
  std::optional<std::filesystem::path> crash_file;    // that process's crash file, if it left one
};

// File line format: `2026-10-01T14:03:22.481Z [warn] [thread] run=.. transport.serial.ig1: msg`
// (UTC wall time, milliseconds; no `run=` outside a run), formatted once on
// the calling thread for the file and the flight recorder. Records are
// written asynchronously by one writer thread (queue 8192, producers block
// when full, nothing is dropped).
// `error` records are on disk when write() returns; everything else within
// about one second, or on flush()/destruction.
//
// A `dir` that cannot be created is not an error: the hub runs without a file
// sink and reports one `error` Log on the bus (stderr if there is no bus).
// An empty `dir` means no file sink, silently.
//
// Levels: a logger's level is the level of the most specific matching rule
// (config `levels`, then set_level()); ties go to the longer pattern, then the
// lexicographically greater one. No match uses `default_level`.
//
// Every record written also goes, on the calling thread, to stderr (when
// `echo_stderr`) as `[level] name: message`, and, when its level is
// `dock_level` or above (the hub's, which set_dock_level moves), to the bus
// (if any) as a `Log` event stamped with
// `clock.now()`. File lines are stamped with `clock.wall_now()`. Bus and clock
// are those attached (attach_bus, attach_clock) while an attachment lives,
// else the ones given to create().
//
// The file is locked (advisory, `<dir>/<file_name>.lock`) for the hub's life.
// A second hub on the same file, in this process or another, writes
// `<stem>-<pid><ext>` instead and says so in one `info` record there.
class LogHub {
 public:
  struct Impl;

  // Fails (ErrorKind::Io) only if the logging back end itself cannot start.
  static Result<std::shared_ptr<LogHub>> create(const config::LoggingConfig& config,
                                                const Clock& clock, SignalBus* bus = nullptr);

  ~LogHub();  // flushes, stops the writer, releases the file lock
  LogHub(const LogHub&) = delete;
  LogHub& operator=(const LogHub&) = delete;

  // A logger named `name` whose level follows the rules. It may outlive the
  // hub; once the hub is destroyed it writes nothing.
  Logger logger(std::string name);

  // Adds or replaces the rule for `pattern` and re-resolves every existing
  // hub logger whose level was not set explicitly. Thread-safe.
  void set_level(std::string_view pattern, LogLevel level);

  // Replaces the default level and every rule at once: a rule that is not in
  // `levels` no longer applies. Explicitly set loggers keep their level, as
  // with set_level. Thread-safe.
  void set_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>& levels);

  // Replaces the recorder's rules: the level at or above which a logger's
  // records are kept by the flight recorder, whatever the file level (spec
  // 5.1). Same glob matching as set_levels; never changes the file level of any
  // logger. Changes nothing when the hub has no ring (recorder off, or its
  // allocation failed): the recorder level then stays off. Thread-safe.
  void set_recorder_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>& levels);

  // The lowest level that goes to the bus (the Log dock); records below it
  // still go to the file. Starts at the config's `dock_level`; a change is
  // in force for the next write on any thread. Thread-safe.
  void set_dock_level(LogLevel level) noexcept;
  [[nodiscard]] LogLevel dock_level() const noexcept;

  // Logs one info record on logger `nobion`: `session start` and `key=value`
  // fields (app, version, os, install, profiles, sim, clock, default_level,
  // dock_level, levels); values with spaces are double-quoted. Marks the hub
  // so its destructor first logs `session end`. Calling it again logs another
  // start line.
  void begin_session(const SessionInfo& info);

  // begin_session also makes `<dir>/<stem>.session` (the file's stem, so the
  // `-<pid>` name of a second hub gets its own) and locks it until the
  // destructor removes it after `session end`. create() looks at every
  // `*.session` in the folder: one whose lock can be taken belongs to a
  // process that is gone without a clean end (a live one's is never
  // reported). Each is logged at `error` (`previous <program> session <build>
  // started <time> ended without a clean exit`, the program the sentinel's)
  // and removed. Its crash file is `crash/<program>-<pid>.txt`, if one was
  // written no earlier than 5 s before the sentinel (a reused pid can name an
  // older process's).

  // The latest session create() found that did not end cleanly, if any.
  std::optional<PreviousSession> previous_session() const;

  // Blocks until every record written so far is on disk.
  void flush();

  // Flight recorder (spec 2026-11-06-field-diagnostics, 5.2-5.4). With
  // `[recorder] enabled`, create() allocates a ring of `size_mb` once. It
  // holds every record that goes to the file and every record at or above
  // its logger's recorder level, as lines of the file's format; the oldest
  // whole lines make room for new ones. A crash writes it at the end of the
  // crash file.

  // The lines the ring holds, oldest first; empty when the recorder is off.
  [[nodiscard]] std::string recorder_snapshot() const;

  // Writes the snapshot to `<dir>/flight/<UTC stamp>-<reason>.log` (stamp
  // from the hub's clock, `20261107T153012.481Z`; of `reason`, control
  // characters dropped everywhere, and in the name its first 64 bytes with
  // characters other than letters, digits, `.`, `_` and `-` written `_`),
  // its first line `flight recorder dump: reason=<reason> build=<build_id>`, and keeps the
  // newest 20 dumps there. Limits, in real time whatever the hub's clock: one
  // dump per reason a minute, one of any reason per 10 s; a call inside a
  // limit writes nothing, logs one `info` line and returns the last dump's
  // path. Fails (Io) with no file folder or when the dump cannot be written,
  // (Config) when the recorder is off. Thread-safe.
  Result<std::filesystem::path> dump_recorder(std::string_view reason);

  // Tests only: the time the dump limits are measured on (steady_clock::now
  // until set).
  void set_recorder_limit_clock_for_testing(std::function<std::chrono::steady_clock::time_point()> now);

  // The bus this hub publishes `Log` events on now (null if none). Producers
  // that own a bus use it to avoid delivering a record twice.
  SignalBus* bus() const noexcept;

  // An attachment of a bus or a clock to the hub; movable, not copyable. Its
  // destructor puts back what create() was given and waits (in real time)
  // until no write still uses what it attached, so the bus or clock may be
  // destroyed right after. It may outlive the hub. Not to be destroyed from
  // inside a write on the same thread (a Log handler of the attached bus):
  // that wait would be for itself.
  class Attachment {
   public:
    Attachment() = default;
    ~Attachment();
    Attachment(Attachment&& other) noexcept;
    Attachment& operator=(Attachment&& other) noexcept;
    Attachment(const Attachment&) = delete;
    Attachment& operator=(const Attachment&) = delete;

   protected:
    friend class LogHub;
    Attachment(std::shared_ptr<Impl> impl, std::uint64_t id) noexcept;

   private:
    void detach() noexcept;
    std::shared_ptr<Impl> impl_;
    std::uint64_t id_ = 0;
  };

  // While it lives, records at `dock_level` and up are published on `bus`
  // (and a handler of `bus` that throws is logged). One bus at a time:
  // attaching another replaces it, and the first attachment's destructor
  // then changes nothing (it still waits for writes that used its bus).
  class BusAttachment : public Attachment {
    friend class LogHub;
    BusAttachment(std::shared_ptr<Impl> impl, std::uint64_t id) noexcept
        : Attachment(std::move(impl), id) {}
  };
  [[nodiscard]] BusAttachment attach_bus(SignalBus& bus);

  // While it lives, file lines are stamped from clock.wall_now() and bus
  // events with clock.now(); after, from the clock given to create(). Same
  // one-at-a-time and wait rules as attach_bus.
  class ClockAttachment : public Attachment {
    friend class LogHub;
    ClockAttachment(std::shared_ptr<Impl> impl, std::uint64_t id) noexcept
        : Attachment(std::move(impl), id) {}
  };
  [[nodiscard]] ClockAttachment attach_clock(const Clock& clock);

  // The file this hub writes (`<dir>/<file_name>`, or the `-<pid>` name when
  // that one is locked); empty when there is no file.
  std::filesystem::path file_path() const;

  // Where a crash of this process is written: `<dir>/crash/<stem>-<pid>.txt`,
  // `<stem>` that of the configured file name; empty when there is no file.
  // create() makes `<dir>/crash` and keeps there the newest 20 crashes (a
  // `.txt` and the `.dmp` of the same name count as one).
  std::filesystem::path crash_file_path() const;

  // Used by Logger; not for callers. Never throws; `message` is written
  // verbatim (it is never interpreted as a format string).
  void write(LogLevel level, std::string_view logger, std::string_view message);

  // Best-effort crash flush (spec 4.5). Static and idempotent, so main() may
  // call it before any hub exists; it acts on whichever hub create() made
  // most recently (held weakly, so a destroyed hub is simply skipped).
  //
  // - std::terminate: writes the active exception text as one line to the
  //   crash file, logs it at `error` on logger `nobion`, flushes (bounded
  //   wait), then calls the previous handler (abort: the SIGABRT handler
  //   then adds its block below that line).
  // - POSIX SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL/SIGTRAP (SA_ONSTACK; the
  //   calling thread is given an alternate stack, other threads take one with
  //   install_signal_stack()): first alarm(5), so a stuck handler still ends
  //   the process; then, with write(2), `fatal signal N`, `build <build_id>`,
  //   `thread <tag>` (LogContext tag, read from a pthread key; `?` if none) and the raw backtrace
  //   (`0x<hex>`, one frame a line) to stderr, to a descriptor on that hub's
  //   log file (moved onto the live file within about 1 s of a rotation) and
  //   to the crash file (crash_file_path(), opened only then); the module map
  //   (`module <path> 0x<load> 0x<size>`) to the crash file, or to stderr
  //   when there is none; then, to the crash file only, a `flight recorder`
  //   line and the ring's lines, read without its lock (recorder_snapshot).
  //   Then restores the default action and re-raises.
  //   No flush (signal-unsafe): records below `error` younger than about
  //   1 s may be lost. Spec 2026-11-06-field-diagnostics, 3.2 and 3.3.
  // - Windows: an unhandled-exception filter writing `fatal exception 0x..`
  //   the same way (the ring after it, to the crash file), returning
  //   EXCEPTION_CONTINUE_SEARCH.
  static void install_crash_handlers();

 private:
  explicit LogHub(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;  // shared with hub loggers, which may outlive the hub
};

}  // namespace nobion
