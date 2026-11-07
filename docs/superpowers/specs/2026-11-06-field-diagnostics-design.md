# Field diagnostics: what a lab install must give back before first deployment

Date: 2026-11-06. Deciders: Jake Ross.
Style: compressed (root `AGENTS.md`). Nothing technical dropped.

## 1. Purpose

nobion not yet deployed. First installs: remote labs, hardware onboarded one instrument at a time, fixes shipped iteratively by one developer far away. Today a fault at a lab leaves too little behind:

- crash: log gets `fatal signal N` or `fatal exception 0x..` only (`libs/core/src/log_hub.cpp`, `on_fatal_signal`, `on_unhandled_exception`); binaries stripped (`cmake/NobionPackaging.cmake:14` `CPACK_STRIP_FILES ON`), `release.yml` builds `Release`, no symbols kept; no git sha anywhere (`AnalysisRecord.instrument.software_git_sha`, `record/types.hpp:48`, never set).
- intermittent driver fault: wire bytes only if `*.wire` at `trace` was on before it happened; spectrometer transports never get a `.wire` logger (`SpectrometerAssembler::default_transport`, `libs/systems/src/spectrometer/assembler.cpp:162-172`, leaves `TransportContext::log_hub` empty).
- hang: nothing notices. `Scheduler::watchdog` (`libs/core/src/scheduler.cpp:106`) has no production caller.
- device going bad: `DeviceHealth` (`libs/devices/include/nobion/devices/device.hpp:24`) tracked, never published or shown; only transport health reaches UI (`HealthBar`).
- getting it off the machine: by hand, file by file.
- power loss: state written tmp then rename, no fsync (`executor.cpp:694`, `extraction_line.cpp:688`, about 20 more hand-written copies; only EarthBank fsyncs).

Goal: when anything goes wrong at a lab, one zip from the operator holds enough to find the cause without a second trip, and the process keeps the minutes before a fault without anyone having turned tracing on.

Read before changing (root `AGENTS.md` table): `libs/metrics/AGENTS.md` (sections 7, 2), `libs/persistence/AGENTS.md` (section 11), `apps/nobion/AGENTS.md` (sections 3, 7, 8), `libs/devices/AGENTS.md` (section 7), `libs/experiment/AGENTS.md` (sections 5, 10), `packaging/AGENTS.md` (section 3.1).

## 2. Build identity

Every artifact says exactly which source built it.

- `cmake/NobionBuildInfo.cmake` generates `build_info.cpp` in the build tree. Runs at configure and again when the git dir's `HEAD` or `index` changes (custom command `DEPENDS` on them; git dir from `git rev-parse --git-dir` at configure, so worktrees work). File rewritten only when content changes: no relink otherwise. Values:
  - `version`: `PROJECT_VERSION`.
  - `git_sha`: `git rev-parse HEAD`, first 12 hex; else env `GITHUB_SHA` (first 12); else `unknown`.
  - `git_dirty`: `git status --porcelain --untracked-files=no` non-empty; false under the `GITHUB_SHA` fallback.
  - `build_type`: `CMAKE_BUILD_TYPE`, or the config under multi-config generators (generator expression).
  - `features`: optional parts compiled in: `scripting`, `vision_opencv`, `publish_parquet`, `persistence`, `metrics`, `hardened_stdlib` (section 4).
- Interface, `libs/core/include/nobion/core/build_info.hpp`:

  ```cpp
  namespace nobion {
  struct BuildInfo {
    std::string_view version, git_sha, build_type;
    bool git_dirty;
    std::span<const std::string_view> features;
  };
  const BuildInfo& build_info() noexcept;
  std::string build_id();  // "0.5.0+3f2a9c1e04b7" or "0.5.0+3f2a9c1e04b7.dirty"
  }
  ```

- `NOBION_VERSION` compile definitions (`apps/nobion/CMakeLists.txt:261,328`, `libs/setup/CMakeLists.txt:19`) removed; their users call `build_info()`. One source of truth.
- Shown in: `nobion --version`, `elctl --version` (`build_id()`); About box and its Copy button (`apps/nobion/src/brand.cpp:68` `build_info()` gains sha, dirty, features); `LogHub::begin_session` line (`build=<build_id>`); metrics `nobion_build_info` gains label `git_sha` (label change checked against `libs/metrics/AGENTS.md`; dashboards in `packaging/observability` updated if they pin the label set); every run record's `instrument.software_version` and `instrument.software_git_sha`, set where the record's instrument block is filled.
- A record's sha is the build that measured it. The importer leaves the field as the source had it.
- Release-please commits version files before the tag, so a release build is clean; `release.yml` fails the job if `build_id()` of the built `elctl` ends `.dirty` or has `unknown`.

## 3. Crash capture

### 3.1 Symbols kept per release

- `release.yml` builds `RelWithDebInfo` (was `Release`): `-O2 -g -DNDEBUG` on gcc/clang (was `-O3`), `/O2 /Zi` + `/DEBUG:FULL` on MSVC, with `/INCREMENTAL:NO /OPT:REF /OPT:ICF` set by `cmake/NobionToolchainFixes.cmake` (RelWithDebInfo's own link flags are `/INCREMENTAL` and drop `/OPT:*`).
- CMake writes `<build>/symbol_files.txt` (`file(GENERATE)`, `$<TARGET_FILE:...>` and on MSVC `$<TARGET_PDB_FILE:...>` of every executable and shared library target of ours). Third-party and bundled libraries (Qt, Python, OpenCV if bundled) not covered: their vendors' symbols serve.
- A `release.yml` step after build, before `cpack`, runs `tools/split_symbols.py <build> <out>` over that list:
  - macOS: `dsymutil` each → `nobion-<version>-macOS-<arch>.dSYM.zip`.
  - Linux: `objcopy --only-keep-debug`, then `objcopy --add-gnu-debuglink` on the build-tree file → `nobion-<version>-linux-x86_64.debug.tar.zst`.
  - Windows: the PDBs → `nobion-<version>-windows-x64.pdb.zip`.
  - The archive uploaded as a release asset beside the installers. Never in an installer.
- `CPACK_STRIP_FILES` stays `ON`: CPack then strips the installed copies (a debuglink section survives strip).
- Repository public, so symbols public: nothing in them the source does not show.
- Release builds run tests too: before packaging, `release.yml` builds and runs `ctest` for `arar`, `reduction`, `processing`, `core`, `vision` suites in the release configuration, so a numeric fixture that differs under `-O2 -g` stops the release (root `AGENTS.md`: a stored format does not depend on the compiler).

### 3.2 What the crash handler writes

Same handlers (`LogHub::install_crash_handlers`), more written: to stderr, the log file descriptor (as today) and the crash file (3.3). Module map (long: hundreds of lines in the app) to crash file only; to stderr when no crash file.

POSIX rules for the handler (signal context): no allocation, no lock, no stdio, no `dladdr`; only preformatted bytes, `write(2)`, `open(2)`, `backtrace()`, `alarm(2)`, `raise(2)`. Everything else captured outside the signal (at install, at `begin_session`, or by the hub's flusher thread).

- Signals: `SIGSEGV SIGABRT SIGBUS SIGFPE SIGILL SIGTRAP` (`SIGTRAP` added: libc++ hardening traps raise it on arm64 macOS; `_GLIBCXX_ASSERTIONS` calls `abort`, `SIGABRT`, already covered). `SA_ONSTACK` set.
- Alternate signal stack (`sigaltstack`, 64 KiB, allocated once per thread): installed for the main thread by `install_crash_handlers`, and for our own threads by `nobion::install_signal_stack()` called first thing in every thread entry we own (`Crew`, `QueuedTransport` worker, `Scheduler` workers, executor, `ngx.reader`, hub writer, hang monitor). A stack overflow on any other thread may still kill the process with no trace.
- First act: `alarm(5)`: a handler stuck for any reason still ends the process.
- Written, in order, after `fatal signal N`:
  - `build <build_id>` (preformatted at install).
  - `thread <tag>`: the thread tag `log_context.hpp` keeps, read without a lock; `?` if none. POSIX handler reads it from a `pthread_key_t` (`pthread_getspecific`) that `LogContext::Thread` sets beside its `thread_local` pointer: on macOS first read of a `thread_local` on a thread that never touched one allocates (dyld TLV), `pthread_getspecific` does not. Key created outside signals (first `Thread` scope, or `install_crash_handlers`). `thread_tag()` for normal callers keeps the `thread_local`.
  - `pc 0x<hex>`: faulting instruction, read from signal's `ucontext_t` (macOS arm64 `__darwin_arm_thread_state64_get_pc`, x86-64 `__rip`; Linux x86-64 `gregs[REG_RIP]`, aarch64 `pc`); line left out where layout unknown. Needed because backtrace frames are return addresses, and macOS's `backtrace()` goes from signal trampoline straight to faulting function's caller (faulting function not in it at all); glibc's has faulting pc itself after trampoline.
  - raw backtrace: `backtrace()` into a static array of 64, written as hex addresses, one a line. `backtrace()` called once at install so its unwinder is loaded before any signal. No symbol names in the handler.
  - module map: name, load address, size of each loaded image, from a static table (1024 entries; past that, counted and written as `modules truncated: N`). Filled outside the signal: macOS: `_dyld_register_func_for_add_image` callback; Linux: `dl_iterate_phdr` at install and again from the hub's flusher thread whenever `dlpi_adds` changes. Two tables, atomic index of the current one: the handler reads a table nobody is writing. Line form `module <path> 0x<load hex> 0x<size hex>`. macOS: load = mach header address (what `atos -l` takes), size = `__TEXT` vmsize (shared-cache images share one `__LINKEDIT` far from their text); one append-only table, entries below an atomic count never rewritten; unloaded image keeps its row. Linux: load = `dlpi_addr` + lowest `PT_LOAD` `p_vaddr` (= `dlpi_addr` for PIE and shared libraries), size = span of `PT_LOAD` segments.
  - flight recorder ring (5.4).
- `std::terminate`: exception text as today, and as one line (`terminate called after throwing: ...`) to the crash file with `open(2)`/`write(2)`; then previous handler, `abort`: SIGABRT handler appends build, thread, backtrace, module map below it. A previous handler that does not abort: crash file holds the text line only.
- Windows: at install, a crash thread is started that waits on an event, and the dump file handle (`<log dir>/crash/<stem>-<pid>.dmp`) is opened then. The unhandled-exception filter writes `fatal exception 0x..`, `build`, thread id, ring (5.4) with `WriteFile`, then signals the crash thread with the `EXCEPTION_POINTERS` and waits up to 10 s; the crash thread calls `MiniDumpWriteDump` (`dbghelp`) with `MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory`. A separate thread because a stack overflow leaves the faulting thread no stack. Best effort: heap corruption can still defeat dbghelp; text lines are written first for that reason. Crash `.txt` handle opened in advance too, beside dump (at install, on retarget, at `begin_session`; `FILE_APPEND_DATA`, `OPEN_ALWAYS`): filter and terminate line only call `WriteFile`, since `CreateFileW` goes through process heap. Dump opened `CREATE_NEW` (file already there, pid reused: no dump this process, old one kept). Both marked delete-on-close (`FileDispositionInfo`; a `.txt` already there is appended to, left unmarked); filter (terminate line) clears `.txt` mark before first write, crash thread clears dump mark before writing, so process that does not crash leaves no empty `.txt` or `.dmp`. Reopened only when path changes; replaced handles closed. Text lines: `fatal exception 0x<code>`, `build <id>`, `thread <tag>` (`thread_local` tag; `?` if none), `thread id <n>`. `install_signal_stack()` on Windows sets `SetThreadStackGuarantee` 64 KiB (failure ignored): main thread at install, every owned thread at entry; filter after stack overflow runs on what is left. Terminate line written to crash file as on POSIX; MSVC `abort()` ends process without filter, so terminate leaves that line and no dump.
- `tools/symbolize.py <crash file> --symbols <dSYM|.debug|pdb dir>`: `pc` line and frame addresses + module map → function, file, line; pc, and a frame equal to it, looked up as is, every other frame at address - 1 (return address: line of the call) (`atos -o <dSYM> -l <load addr>` on macOS; `llvm-symbolizer` or `addr2line` with the module offset on Linux). For a `.dmp` it prints the WinDbg / Visual Studio steps.

### 3.3 Crash file and unclean exit

- Crash directory `<log dir>/crash/` created by `LogHub::create`. Crash file path `<log dir>/crash/<stem>-<pid>.txt` preformatted into a static buffer at install, re-set by `retarget_crash_output` (`log_hub.cpp`) and at each `begin_session`. Opened in the handler (`O_CREAT|O_WRONLY|O_APPEND`) only when a crash happens. At hub start, crash files and dumps past the newest 20 removed.
- Session sentinel: `begin_session` creates `<log dir>/<stem>.session` (pid, build id, start time from hub clock's `wall_now()`) and holds an exclusive advisory lock on it for the session (`flock`; Windows `LockFileEx`). Clean end (`~LogHub`, after `session end`) unlocks and removes it. At start, for each `*.session` in the log dir: if its lock can be taken, its process is gone and did not end cleanly (a live process holds its lock; a reused pid does not matter). Then:
  - program: sentinel's own stem, less a trailing `-<pid>` equal to its `pid=` (second hub's `elctl-4242.session` → `elctl`). Never reporting hub's stem: programs share a log dir, so nobion hub can find elctl's sentinel;
  - crash file: `<log dir>/crash/<program>-<pid>.txt`, only if its mtime is no earlier than sentinel file's mtime minus 5 s (reused pid can name older process's crash file). Sentinel's mtime, not its `started=`: `started=` is hub clock's `wall_now()` (simulated in simulated session), mtime same file-system clock as crash file's;
  - logged at `error`: `previous <program> session <build> started <time> ended without a clean exit`, with the crash file path if there is one; the stale sentinel removed;
  - `LogHub::previous_session()` → `std::optional<PreviousSession>{program, build_id, started, crash_file}`;
  - nobion: once the main window shows, a dialog "<program> did not close cleanly last time", buttons **Collect Diagnostics…** (section 8) and **Dismiss**;
  - elctl: one line on stderr.
- A second process on the same log dir (`-<pid>` file name) has its own stem, so its own sentinel.

## 4. Hardened standard library

Our code built with the standard library's cheap checks: out-of-range index, empty optional dereferenced, bad iterator become a clean abort (and crash file) instead of silent corruption.

- libstdc++: `_GLIBCXX_ASSERTIONS` (aborts: `SIGABRT`).
- libc++: configure check is for the capability, not a version: `<version>` must define `_LIBCPP_HARDENING_MODE_FAST` (upstream libc++ 18+; upstream 17 used `_LIBCPP_ENABLE_HARDENED_MODE` and fails the check). Check decides, not a compiler or version list: Apple clang on developer's Mac (Apple clang 21, `_LIBCPP_VERSION` 210106) has the modes. Check sees same headers build does: `CMAKE_REQUIRED_FLAGS` carries `-nostdinc++ -isystem <SDK libc++>` when `cmake/NobionToolchainFixes.cmake` swapped them in. "Hardened" means value of `_LIBCPP_HARDENING_MODE` other than `_LIBCPP_HARDENING_MODE_NONE`, never `defined(_LIBCPP_HARDENING_MODE)`: `<version>` defines it to its default (none) with nothing asked. Passes: `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` (traps: `SIGILL` x86-64, `SIGTRAP` arm64); fails: none.
- MSVC: none (`_ITERATOR_DEBUG_LEVEL` changes ABI; Qt built at 0). About features say `hardened_stdlib=off` there.
- `cmake/NobionHardening.cmake` applies them to `nobion_*` targets and both apps, every build type (a dev build fails the way a lab build does). Not applied when `NOBION_SANITIZE` is set: the sanitizer setup already turns on the library checks (root `AGENTS.md`) and wins.
- Claim: no change to class layout in either library, so prebuilt Qt and Homebrew libraries link unchanged. Inline functions instantiated by both our code and a third-party target get different bodies (ODR); benign in practice, and the full `dev-ui` suite on the macOS UI CI job under the macros is the check.
- Cost measured before turning on (section 14).

## 5. Flight recorder

Last minutes of everything, in memory, written out when something goes wrong.

### 5.1 What is kept

- `LogHub` gains a second threshold per logger, the recorder level, resolved like file levels (glob rules, most specific wins) from `[recorder]` in `logging.toml`:

  ```toml
  [recorder]
  enabled = true          # default true
  size_mb = 32            # ring bytes, default 32, 4..512
  default_level = "debug"
  [recorder.levels]
  "*.wire" = "trace"      # default: wire bytes always kept
  ```

- `Logger` (`logger.hpp`, state packed `(epoch << 9) | (explicit << 8) | level`): `level()` stays the file level. A second cached value, `gate` = min(file level, recorder level), packed the same way; `enabled(l)` compares against `gate`.
- `log(l, m)`: file, stderr echo and bus (`Log` at or above `dock_level`) only when `l >= level()`, exactly as today; ring when `l >=` recorder level. Bus event records (`trace.<Event>`) follow the same rule.
- `Logger::set_level` (explicit) changes the file level only; the recorder level follows recorder rules only.
- Cost: a record below the file level but at the recorder level is now formatted (hex for `*.wire`). Measured (section 14); if over budget, `.wire` loggers format into the ring only, skipping the file formatter's work.

### 5.2 The ring

- One byte ring of `size_mb`, allocated once at hub start; its pointer never changes. Lines as the file writes them (same format, stamp). Producer formats into a thread-local buffer, then copies under a mutex held only for the copy; head and tail indices `std::atomic<std::uint64_t>`. Oldest whole lines overwritten. A record that reaches file and ring formatted once.
- `LogHub::recorder_snapshot()` → `std::string`, oldest first, whole lines only.

### 5.3 When it is written out

`LogHub::dump_recorder(std::string_view reason)` → `Result<std::filesystem::path>`: snapshot to `<log dir>/flight/<UTC stamp>-<reason>.log`, first line `flight recorder dump: reason=<reason> build=<build_id>`. Keeps the newest 20.

Limits, on `steady_clock` (real time, whatever the hub's clock): one dump per exact reason per 60 s; any reason at most one per 10 s. A call inside a limit writes nothing, logs one `info` line, returns the last dump's path. So a storm keeps its first dump.

`FlightRecorderTriggers` (`libs/core` `flight_recorder_triggers.hpp`): owns one worker thread (real time, joined in its destructor) that calls `dump_recorder`, so no bus handler does file I/O on the publishing thread.

```cpp
class FlightRecorderTriggers {
 public:
  FlightRecorderTriggers(LogHub& hub, SignalBus& bus);  // subscribes Alarm, DeviceHealthChanged
  void request(std::string reason);                     // queue a dump; thread-safe, never blocks
};
```

Subscriptions and reasons:
- `Alarm` (`libs/core/include/nobion/core/events.hpp:63`) with `severity == Critical`: `alarm-<source>`.
- `DeviceHealthChanged` to `Faulted` (section 7): `faulted-<device>`.
- `RunFinished` (`libs/experiment/include/nobion/experiment/executor/executor.hpp:129`) with a failed state, and `QueueEnded` (`libs/experiment/include/nobion/experiment/lab/session.hpp:86`) with end `unsafe`: subscribed by the experiment side (`libs/experiment` wires these to `request`): `run-failed-<run id>`, `unsafe`.
- Hang monitor (section 6): `hang-<beat>`, calling `request`.
- Operator: nobion Help > Save Flight Recorder (`manual`); support bundle (`bundle`, synchronous `dump_recorder`).

Both apps construct it in `main` after the hub and bus.

### 5.4 On a crash

Signal handler (and Windows filter) writes the ring's two contiguous spans to the crash file with `write(2)`, reading the atomic indices, without the mutex. A line torn by a concurrent writer is accepted.

## 6. Hang monitor

`libs/core` `hang_monitor.hpp`. One thread, real time on purpose: it watches threads that may hold the virtual clock. Added to the real-time list in `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` §4.4 in the same branch.

```cpp
class HangMonitor {
 public:
  struct Options { std::chrono::milliseconds poll{1000}; };
  HangMonitor(LogHub& log, FlightRecorderTriggers& recorder, SignalBus& bus, Options o = {});
  ~HangMonitor();  // stops and joins
  class Beat {     // movable; destructor unregisters
   public:
    void pulse() noexcept;  // lock-free: atomic store of steady_clock::now(); arms a disarmed beat
    void disarm() noexcept; // not watched until the next pulse
  };
  Beat beat(std::string name, std::chrono::milliseconds limit);  // starts armed
};
```

- Not `Scheduler::watchdog`: that runs on the injected clock and the scheduler pool, which are among the things that hang.
- An armed beat silent past its limit, in order: one `error` record `hang: <name> silent for <s> s` on logger `hang`; `recorder.request("hang-<name>")`; then a warning `Alarm{source = "hang"}` published from the monitor thread (last, since a hung bus handler may block the publish). Once per episode: next report only after a pulse.
- Beats:
  - `ui.main`: a `QTimer` 1 s on the main thread pulses; limit 10 s. Always registered (Qt timers are real time).
  - `scheduler`: an `every("hang.beat", 1 s)` job pulses; limit 30 s. Registered only on a `SteadyClock`.
  - `transport.<name>`: `QueuedTransport` pulses when a call starts and disarms when it ends; idle transports unwatched; its waits unchanged (clock rules). Limit `max(30 s, 10 × timeout × (retries + 1))`. Registered only on a `SteadyClock`: under a `VirtualClock` simulated waits have no fixed real-time length.
  - executor: none (waits on purpose for hours); a run stuck in one phase stays the metrics `NobionStuck` alert's job.
- Stacks of the hung thread: out of scope (no portable in-process way). `docs/installation_runbook.md` and the bundle README give per-OS commands: `sample <pid> 10` (macOS), `gdb -p <pid> -batch -ex "thread apply all bt"` or `eu-stack -p <pid>` (Linux), `procdump -ma <pid>` (Windows).

## 7. Device health published and shown

- `DeviceOptions` gains `SignalBus* bus = nullptr`. Set wherever lines and spectrometers build devices (`ExtractionLine`'s driver construction, `SpectrometerAssembler::default_driver`, laser bring-up); the plan lists each site. Null: nothing published (bare-driver tests).
- On a change of `DeviceState` only (not every failure), `Device` publishes `DeviceHealthChanged{device, transport, from, to, consecutive_failures, last_error_kind, last_error_text, ts}` (new, `libs/core` `events.hpp`). `last_error_text` is the last failure's `Error::what`, carried in the event only; `DeviceHealth` itself unchanged (stays lock-free).
- Published on the calling thread (a transport worker or the caller of the driver). Rule: a `DeviceHealthChanged` handler never calls into a device or transport; it copies and returns. Handlers here: systems event recorder, metrics, `CoreBridge` (queued to the UI thread), `FlightRecorderTriggers` (queues).
- Systems event recorder logs it: `warn` to `Degraded`/`Faulted`, `info` back to `Ok`.
- Metrics: `nobion_device_state{device,transport}` (0 unknown, 1 ok, 2 degraded, 3 faulted), `nobion_device_failures_total{device}`; series removed when the device is torn down, by the series rules of `libs/metrics/AGENTS.md`.
- UI: a transport's `HealthBar` chip turns amber when the transport is up but one of its devices is `Degraded` or `Faulted`; tooltip lists each device on it: state, consecutive failures, last error text, last ok ("ago" from `CoreBridge::now()`, root `AGENTS.md` time rules).

## 8. Support bundle

One zip a lab sends back.

- `libs/setup` `support_bundle.{hpp,cpp}`; zip via miniz (new `PRIVATE miniz::miniz` link of `nobion_setup`, as `libs/earthbank` does).

  ```cpp
  struct BundleOptions {
    std::filesystem::path out;               // default <log dir>/bundles/nobion-<host>-<UTC stamp>.zip
    std::chrono::hours logs_since{72};       // rotated logs older than this left out
    std::uint64_t max_bytes = 200ull << 20;  // over it: newest content kept, oldest dropped, README says what
    bool include_traces = true;
    std::function<std::string()> live;       // running app's extra state (JSON); null in elctl
  };
  Result<std::filesystem::path> write_support_bundle(const setup::Install*, const BundleOptions&);
  ```

- Contents (zip paths):
  - `README.txt`: what this is, build id, host, OS, when, what was left out and why, hang-stack commands (section 6).
  - `build.json`: `build_info()`, OS, CPU, Qt version (nobion), `site.toml` path.
  - `logs/`: current and rotated nobion and elctl logs newer than `logs_since`; `crash/` files and `.dmp`; `flight/` dumps; a fresh `dump_recorder("bundle")` when the process has a hub.
  - `config/`: every file of the install root (`*.toml`, canvas, scripts, plans, queues), each `X.local.toml`, `logging*.toml`, `sim.toml`, `notifications.toml`; `install.toml`; `config/EDITED.txt` listing files whose sha256 differs from `install.toml`'s `files` map (`libs/setup/include/nobion/setup/install.hpp:73`); a file missing from the map (older install) listed as `unknown`.
  - `state/`: line `*.state.toml`, `executor_state.json`, field table `current` and its version file, outbox and spool listings (names, sizes, ages; no record bodies).
  - `traces/`: `<trace dir>/*.trace`, newest first, within the cap, when `include_traces`.
  - `doctor.txt`: `elctl doctor` report (no `--probe`: never touches hardware).
  - `live.json`: from `live`: transport and device health, active alarms and the last 100, executor state, metrics text (registry render), current log and recorder levels.
- Redaction, `libs/core` `redact.hpp` (public; replaces the private `is_secret_key` in `libs/core/src/log_summary.cpp`, which then calls it):
  - key rule: a key equal to, or ending in `_`/`-` plus, one of `password`, `passwd`, `token`, `secret`, `key`, `credential`, `api_key` (case-insensitive; the existing suffix rule, list extended). Value replaced by `***` (the existing text).
  - value rule: in any text, `<scheme>://<user>:<password>@` becomes `<scheme>://<user>:***@`.
  - Never copied: `.nobion/credentials.toml`, keychain items.
  - Applied to every text file in the bundle (TOML by key and value rule; logs, JSON, traces' comment lines, `live.json` by value rule; TOML keys inside logged `key=value` summaries already redacted at log time).
- Entry points:
  - `elctl support-bundle [--install X] [--out file] [--since 72h] [--no-traces]`.
  - nobion Help > Collect Diagnostics…: snapshot for `live` taken on the main thread from `CoreBridge`/`ExperimentBridge`, then the bundle written on a worker thread with progress; the zip revealed in the file manager.
  - the unclean-exit dialog (3.3).
- Never uploads. Sending is the operator's (email, share). Upload: section 13.

## 9. Spectrometer wire log

- `SpectrometerContext` (`libs/systems/include/nobion/systems/spectrometer/spectrometer.hpp:149`) gains `std::shared_ptr<LogHub> log_hub` (same type as `TransportContext::log_hub`, `libs/transport/include/nobion/transport/factory.hpp:19`); `SpectrometerAssembler::default_transport` passes it on. Both apps set it.
- Qtegra and NGX transports then log `<name>.wire` like line transports, and the flight recorder keeps their bytes by default (5.1).

## 10. Durable file writes

- `libs/core` `atomic_file.hpp`:

  ```cpp
  enum class Durability { Crash, Power };
  // Temp file beside `path` (same file system), write, flush, rename over `path`.
  // Crash: survives a process crash (no sync).
  // Power: survives power loss: POSIX: fsync file (macOS: fcntl F_FULLFSYNC, as
  //   SQLite does, since fsync there does not flush the drive cache), rename,
  //   fsync directory. Windows: FlushFileBuffers, then
  //   MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), retried
  //   5 times 50 ms apart on a sharing violation or access denied (antivirus, preview), then Io.
  // Either the old content or the new is left at `path`, never a mix or nothing.
  Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view bytes,
                                 Durability d = Durability::Power,
                                 std::optional<std::filesystem::perms> new_file_mode = std::nullopt);
  ```
  `new_file_mode`: bits (masked to 0777) given to a file made where none exists (stat fails, dangling link included), set on the temp before any bytes are written; an existing file keeps its bits; Windows ignores it.

- Moved onto it at `Power` (by function):
  - `Executor` state write (`executor.cpp:694`), `ExtractionLine` state write (`extraction_line.cpp:688`);
  - `persist/persister.cpp` `write_atomic` (spool and records, its rename at `:42`);
  - notifier outbox (`lab/notifier.cpp:289`);
  - `field_table_store.cpp` `write_atomic` (rename at `:54`);
  - laser `calibration_store.cpp`, `correction_store.cpp`, `camera_scale.cpp`, `pattern.cpp`;
  - `config/local_file.cpp:248`, `setup/site.cpp:131`, `model/queue_file.cpp:188`, `conditionals/library.cpp:105`, `factory/step_heat_template.cpp:477`;
  - EarthBank `save_profiles` (`profiles.cpp`).
- Left alone: EarthBank journal (append with its own fsync), `uploader.cpp:303` (a rename aside, not a write), PNG, OOXML and report exports, `endpoints.cpp`, processing `options.cpp`, isotope classifier cache, script editor save (operator re-saves).
- Rule: a new writer of state the process reads back after restart uses `write_file_atomic(..., Power)`. `tools/check_atomic_writes.py` (run by `tools/githooks/pre-push` with `quality_check.py`) fails on a `rename(` in `libs/` or `apps/` outside `atomic_file.cpp` and an allowlist file naming the left-alone sites above.
- Line state is written on every actuation. If `Power` costs over 20 ms a write on the test Mac (measured), line state writes coalesce: latest state, at most one write per 250 ms, flushed on stop. Acceptable because on restart the hardware read-back is the truth for actuated valves (`extraction_line.hpp:86-97`); a manual-valve change is never coalesced (written at once).

## 11. Store backup before migration

Every open with migration on goes through `persistence::detail::migrate` (`libs/persistence/src/migrate.cpp`): `elctl db migrate`, and the creating/migrating opens of `elctl init`/setup (`apps/elctl/src/setup.cpp:165`), `elctl entry` seeding (`apps/elctl/src/entry.cpp:599`), `elctl import` (`apps/elctl/src/import.cpp:252`), nobion's setup wizard (`apps/nobion/src/setup_support.cpp:45`). The backup goes there, so all of them get it.

- `StoreConfig` gains `bool backup_before_migrate = true`.
- In `migrate`, when `apply` and `backup_before_migrate`, before the first transaction: read applied versions; if the store already has applied migrations and at least one known migration is not applied:
  - SQLite file store: `VACUUM INTO '<db dir>/<db stem>.pre-<from>-to-<to>-<UTC stamp>.sqlite'` (outside any transaction; `<from>` highest applied, `<to>` highest known; single quotes in the path doubled; refused if the target exists). Path logged at `info` on logger `store` and returned to the caller through a new `MigrationReport{backup}` the CLI prints. A failed backup refuses the migration (`Io`).
  - In-memory SQLite: no backup.
  - PostgreSQL: no backup by the library. `elctl db migrate` refuses an existing PostgreSQL store with pending migrations unless `--backed-up`; the refusal prints a `pg_dump --format=custom` command for the URL without its password. Release notes of the version that adds this say so.
- A fresh store (nothing applied): no backup.
- Tests that migrate temporary stores leave the default (backup beside a temp file is harmless); CLI tests on PostgreSQL pass `--backed-up`.

## 12. Optional libraries in release packages

Owner, 2026-11-06: ship OpenCV, built trimmed; ship without Arrow for now.

- Measured on the developer Mac: Homebrew `opencv` pulls 105 packages (ffmpeg, vtk, openvino, hdf5, boost, ...), 156 MB of libraries; `apache-arrow` 32 packages, 115 MB. Bundled through `macdeployqt` that would add hundreds of MB to `Nobion.app`, every dylib signed and notarized, and Homebrew bottles only run on the macOS they were built for (runner macOS 15). Not used.
- Code needs only OpenCV `core`, `imgproc`, `videoio` (`cmake/NobionDependencies.cmake:73`; `geometry` too on 5.x).

### 12.1 OpenCV, trimmed and static

- `packaging/opencv/build.py <src> <prefix>`: configures, builds and installs a pinned OpenCV 4.x release (version in the script; tarball checked by sha256) into `<prefix>`. One script for the three OSes. Options:
  - `BUILD_LIST=core,imgproc,videoio`, `BUILD_SHARED_LIBS=OFF`, `BUILD_WITH_STATIC_CRT=OFF` (MSVC `/MD`, as ours), `CMAKE_POSITION_INDEPENDENT_CODE=ON`, `CMAKE_BUILD_TYPE=Release`.
  - Camera backends on: `WITH_AVFOUNDATION` (macOS), `WITH_V4L` (Linux), `WITH_MSMF` and `WITH_DSHOW` (Windows).
  - Off: `WITH_FFMPEG`, `WITH_GSTREAMER`, `WITH_TBB`, `WITH_OPENMP`, `WITH_OPENCL`, `WITH_IPP`, `WITH_ITT`, `WITH_PROTOBUF`, `WITH_EIGEN`, `WITH_LAPACK`, image codecs (`WITH_JPEG`, `WITH_PNG`, `WITH_TIFF`, `WITH_WEBP`, `WITH_OPENJPEG`, `WITH_OPENEXR`, `WITH_JASPER`, `WITH_IMGCODEC_*`), `BUILD_TESTS`, `BUILD_PERF_TESTS`, `BUILD_EXAMPLES`, `BUILD_DOCS`, `BUILD_opencv_apps`, `BUILD_JAVA`, `BUILD_opencv_python3`. `BUILD_ZLIB=ON` (bundled zlib, static).
  - macOS: `CMAKE_OSX_DEPLOYMENT_TARGET` and `CMAKE_OSX_ARCHITECTURES` as the app's.
- `release.yml`: restores `<prefix>` from `actions/cache` keyed on OS, arch, OpenCV version and the script's sha256; on a miss runs the script. Then configures nobion with `-DNOBION_VISION_OPENCV=ON -DOpenCV_DIR=<prefix>/<cmake dir>` (`ON`: the release fails if it is not found, never a silent stub).
- Static: nothing new to bundle, sign, notarize or depend on in the `.deb`. The frameworks and system libraries `videoio` needs (AVFoundation, CoreMedia, CoreVideo; Media Foundation) are linked through OpenCV's exported config.
- Project CMake unchanged: no fetch fallback (`NobionDependencies.cmake` comment: a source build is too heavy for CI). Dev and CI keep Homebrew's (`ci.yml` `ui` job).
- OpenCV is Apache-2.0: its license text added to the installer's third-party notices (`packaging/`).
- Measured in plan: installer size and release job time, before and after, cache cold and warm.

### 12.2 Arrow

- `release.yml` sets `-DNOBION_PUBLISH=OFF` explicitly (was `AUTO`, found nothing). A release made by a packaged build writes everything but `data/*.parquet` (`libs/publish`), as today.
- About `features` shows `publish_parquet=off`; release notes, and a line added to `docs/release_format.md`, say Parquet tables need a build with Arrow.
- Revisited when hosted publishing (data publishing spec, 2026-10-18) is built; possibly a separate data-reduction build rather than the instrument installer.

## 13. Out of scope (later groups)

- Upload of bundles, telemetry, crash upload (Crashpad, Sentry server). Bundle and dump formats chosen so a later uploader sends them unchanged.
- Remote diagnostic API in the app, `elctl --attach`, remote level changes.
- Device console (raw commands), single-driver bench mode, trace → test tool, sim-serve trace replay, fault injection from `sim.toml`.
- Notification events for unclean exit, device fault, hang (notifier lives in a queue's `LabSession`; own spec).
- Splitting control core from UI; process supervisor; safe state at startup after a crash.
- Stacks of other threads at a hang (6).
- JSON log sink; age-based log retention.
- Release channels; auto-update.
- Windows code signing; macOS Intel build.
- Apache Arrow in release packages (12).

## 14. Rules that must hold, and how each is shown

- One build id in every artifact (binary, session line, About, metrics, run record, crash file, bundle). Release builds never `unknown` or `.dirty` (release job check, 2).
- POSIX handler uses only the calls listed in 3.2. Shown by test: a helper executable (`tests/core/crash_helper.cpp`, `posix_spawn`ed by `tests/core/test_crash_capture.cpp`) dies by each signal in turn, by a stack overflow on the main thread and on a thread with `install_signal_stack()`, and by an out-of-range `std::vector::operator[]` under the hardening macros; the parent reads the crash file: `fatal signal`, `build`, `thread`, at least 3 frame addresses, a module map entry for the helper, ring lines.
- Symbolization: a `release.yml` step symbolizes a crash of the packaged helper (`elctl` gains no crash command; the helper is built in the release job) against the split symbols and checks a frame names `crash_helper.cpp`. Not in ctest.
- Windows: the helper's `.dmp` opens with `MiniDumpReadDumpStream` (Windows CI job).
- Unclean exit detected exactly when a sentinel's lock can be taken; a live process's sentinel never counts (test: two processes).
- Recorder: a `trace` record of `*.wire` with file level `info` is in the snapshot and not in the file; `Logger::level()` still returns the file level; ring never exceeds `size_mb`; a second dump with the same reason within 60 s writes nothing; any reason within 10 s writes nothing.
- Recorder cost: example lab under `nobion --sim` with `*.wire` recorded, CPU and records a second, before and after: at most 2% CPU added (root `AGENTS.md`: a change with a cost comes with a measurement).
- Hardened stdlib, and `RelWithDebInfo -O2` vs `Release -O3`: `ctest --preset dev` wall time and an example queue at `--sim-speed max`, before and after, recorded in the plan; over 3% on either: back to the owner before turning on.
- Hang monitor: a test blocks a beat's thread (2 s limit) and sees one `hang:` record, one dump request, one `Alarm`; a pulse re-arms; a disarmed beat never reports.
- Device health: a fake transport failing `fault_after` times gives exactly one `DeviceHealthChanged` to `Faulted`; recovery one to `Ok`.
- Bundle from the example install holds each section; no seeded secret (key rule and URL rule) appears anywhere in the unzipped bytes (test greps them all); `max_bytes` honoured.
- `write_file_atomic`: a test seam fails each step in turn; `path` holds old or new content after each, never empty.
- `elctl db migrate` on a SQLite store with a pending migration leaves a backup that opens with the old schema version; a fresh store gets none.

## Open questions

None. (OpenCV and Arrow in releases: settled by the owner 2026-11-06, section 12.)
