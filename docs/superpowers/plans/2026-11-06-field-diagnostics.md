# Field Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Before the first lab install, make every fault at a lab leave behind enough evidence (build identity, crash file with a symbolizable backtrace, the minutes before the fault, device health, a support bundle) to find its cause remotely, and make state survive power loss.

**Architecture:** Most of the work is in `libs/core` (build info, durable writes, crash handler, flight recorder ring inside `LogHub`, hang monitor, redaction), with consumers in `libs/devices` (health events), `libs/setup` (support bundle), `libs/persistence` (backup before migration), both apps, and the release workflow (symbols, trimmed static OpenCV). Eight branches, each landing on `develop` alone.

**Tech Stack:** C++20, CMake, GoogleTest, Qt 6 (Qt Test for UI), spdlog (behind `LogHub`), miniz, Python 3 tools, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-06-field-diagnostics-design.md` (read it whole before starting; section numbers below refer to it).

## Global Constraints

- Read before changing (root `AGENTS.md` table): `libs/metrics/AGENTS.md` (Tasks 2, 14), `libs/persistence/AGENTS.md` (Task 20), `apps/nobion/AGENTS.md` (Tasks 2, 7, 12, 15, 19), `libs/devices/AGENTS.md` (Task 14), `libs/experiment/AGENTS.md` (Tasks 2, 4, 12), `packaging/AGENTS.md` (Tasks 8, 21).
- Errors are values (`nobion::Result<T>`); no exceptions out of `libs/`.
- Time rules of root `AGENTS.md` hold. Real time on purpose, and added to virtual clock spec §4.4 in the branch that adds them: the hang monitor thread, the `FlightRecorderTriggers` worker, the dump rate limits (`steady_clock`).
- Build id format: `<version>+<12 hex>` or `<version>+<12 hex>.dirty`; `unknown` sha only with neither git nor `GITHUB_SHA`.
- POSIX crash handler may call only: `write(2)`, `open(2)`, `backtrace()`, `alarm(2)`, `raise(2)`, `signal`/`sigaction` to restore the default. No allocation, no lock, no stdio, no `dladdr`, no `backtrace_symbols_fd`.
- Fatal signals: `SIGSEGV SIGABRT SIGBUS SIGFPE SIGILL SIGTRAP`, with `SA_ONSTACK`.
- Crash files: `<log dir>/crash/<stem>-<pid>.txt` and `.dmp`; newest 20 kept. Flight dumps: `<log dir>/flight/<UTC stamp>-<reason>.log`; newest 20 kept; one per exact reason per 60 s, any reason one per 10 s.
- Recorder defaults: `enabled = true`, `size_mb = 32` (range 4..512), `default_level = "debug"`, `"*.wire" = "trace"`.
- Redaction: keys `password passwd token secret key credential api_key` (equal, or suffix after `_`/`-`, case-insensitive) → `***`; `<scheme>://<user>:<password>@` → `<scheme>://<user>:***@`.
- Bundle defaults: `logs_since = 72h`, `max_bytes = 200 MiB`, `include_traces = true`.
- `write_file_atomic(..., Durability::Power)`: macOS `F_FULLFSYNC`; Windows `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` retried 5 times 50 ms apart.
- Each branch lands by the root `AGENTS.md` workflow: rebase on `origin/develop`, `python3 tools/quality_check.py`, `python3 tools/compiler_sweep.py --changed` (`--build-dir build/dev-ui` when `apps/nobion` or `tests/ui` changed), full `ctest --preset dev` (or `dev-ui`), push. Code that adds a thread (Tasks 12, 13) is run under `-DNOBION_SANITIZE=thread` before it lands.
- `tests/core/CMakeLists.txt` and `apps/elctl/tests/CMakeLists.txt` list sources by hand: every new test file in this plan is added there in its task (UI tests under `tests/ui` are globbed). Task 2 onward touches `apps/nobion`: use `dev-ui` for those.
- CI does not run on work branches or `develop`. Windows-only and release-workflow steps (Tasks 6, 8, 21) are first exercised by the next `develop`→`main` pull request; a red run there is fixed on `develop` before other work lands (root `AGENTS.md`).
- Every measurement the spec asks for (section 14) is recorded in this plan file under the task that makes it, before and after.

## Branch order

Branches 1, 2, 7 and 8 depend on nothing here and may land in any order. Branch 3 (crash capture) needs 1 (build id in the crash file). Branch 4 (flight recorder) needs 3: Task 11 extends the crash handler. Branch 5 needs 4 (Task 14 subscribes the recorder triggers). Branch 6 needs 3 and 4 (the unclean-exit dialog, `dump_recorder`). Each branch is cut from `origin/develop` after the branches it needs have landed.

## Review Focus

1. **A crash on a thread our code did not start (Qt, Python, OpenCV), or a crash while the log hub is being destroyed.** Expect: crash file still written (no alternate stack there, so a stack overflow on such a thread may be lost; any other signal is caught). Test in Task 5: helper crashes in a raw `std::thread` with no `install_signal_stack()` by null dereference; crash file has `fatal signal 11` and frames.
2. **Two nobion processes on one log dir (the `-<pid>` log name), one of which crashes.** Expect: the surviving process does not report the live one as unclean; on next start only the dead one is reported. Test in Task 7 (`ALiveSecondProcessIsNeverReportedUnclean`).
3. **A secret in a place the key rule does not see**: a password inside a URL in a log line, `site.toml` `database = "postgres://u:pw@h/db"`, a token in `notifications.toml` under an unusual key ending `_key`. Expect: absent from every byte of the bundle. Test in Task 18 seeds all three.
4. **Disk full or read-only log dir** when a dump, crash file or bundle is written. Expect: `Io` error returned or logged once, the process keeps running, no partial file left as the newest dump. Tests in Task 11 (`ADumpToAReadOnlyDirectoryFailsCleanly`) and Task 3 (`AFailedWriteLeavesTheOldFile`).
5. **A storm**: a gauge flapping Degraded↔Faulted every second, or 30 alarms in a burst. Expect: at most one dump per 10 s, first one kept, one log line per suppressed request, no UI freeze. Test in Task 12 (`AStormKeepsItsFirstDump`).

---

## Branch 1: `feat/core-build-identity` (Tasks 1–2)

### Task 1: Build info generated at build time

**Files:**
- Create: `cmake/NobionBuildInfo.cmake`, `cmake/build_info.cpp.in`, `libs/core/include/nobion/core/build_info.hpp`
- Modify: `CMakeLists.txt` (include the module), `libs/core/CMakeLists.txt` (add generated source)
- Test: `tests/core/test_build_info.cpp`; `tests/core/CMakeLists.txt` (source and the two `NOBION_TEST_*` definitions)

**Interfaces:**
- Produces: `nobion::BuildInfo{std::string_view version, git_sha, build_type; bool git_dirty; std::span<const std::string_view> features;}`, `const BuildInfo& nobion::build_info() noexcept`, `std::string nobion::build_id()`, `bool nobion::has_feature(std::string_view)`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/core/CMakeLists.txt gives the test target NOBION_TEST_PROJECT_VERSION="${PROJECT_VERSION}"
// and NOBION_TEST_HAS_METRICS=$<BOOL:${NOBION_METRICS}>.
TEST(BuildInfo, VersionIsTheProjectVersion) { EXPECT_EQ(nobion::build_info().version, NOBION_TEST_PROJECT_VERSION); }
TEST(BuildInfo, ShaIsTwelveHexOrUnknown) {
  const auto sha = nobion::build_info().git_sha;
  EXPECT_TRUE(sha == "unknown" || (sha.size() == 12 && std::ranges::all_of(sha, ::isxdigit)));
}
TEST(BuildInfo, IdJoinsVersionAndSha) {
  const auto& b = nobion::build_info();
  EXPECT_EQ(nobion::build_id(), std::string(b.version) + "+" + std::string(b.git_sha) + (b.git_dirty ? ".dirty" : ""));
}
TEST(BuildInfo, FeaturesNameWhatWasCompiledIn) {
  EXPECT_EQ(nobion::has_feature("metrics"), NOBION_TEST_HAS_METRICS != 0);  // definitions given by tests/core/CMakeLists.txt
}
```

- [ ] **Step 2: Run** `cmake --build --preset dev --target nobion_core_tests` — expect a compile failure (no header).
- [ ] **Step 3: Implement.** `NobionBuildInfo.cmake` defines `nobion_generate_build_info(<target>)`: at configure, `git rev-parse --git-dir` (absent: no git); an `add_custom_command(OUTPUT <build>/generated/build_info.cpp ... DEPENDS <gitdir>/HEAD <gitdir>/index cmake/build_info.cpp.in)` running `cmake -P` on a script that computes sha (12 hex; else `$ENV{GITHUB_SHA}`; else `unknown`), dirty (`git status --porcelain --untracked-files=no`), and writes the file with `configure_file(... )` semantics (only when content differs). `build_type` is the generator expression `$<CONFIG>` passed as a compile definition to that one source. `features` from `NOBION_SCRIPTING`, `NOBION_VISION_OPENCV_ENABLED`, `NOBION_PUBLISH_ARROW_ENABLED`, `TARGET nobion_persistence`, `NOBION_METRICS`, `NOBION_HARDENED_STDLIB` (Task 9 sets it; default off).
- [ ] **Step 4: Run** `build/dev/tests/core/nobion_core_tests --gtest_filter='BuildInfo.*'` — PASS. Edit a tracked file, `git update-index --refresh` (the command depends on the index, which an edit alone does not touch), rebuild: `git_dirty` true; revert, refresh, rebuild: false. Rebuild again unchanged: `ninja -C build/dev -n` shows no relink of `nobion_core`.
- [ ] **Step 5: Commit** `feat(core): build info names the git sha and the features built in`

### Task 2: Every artifact names the build

**Files:**
- Modify: `apps/nobion/CMakeLists.txt:261,328`, `libs/setup/CMakeLists.txt:19` (drop `NOBION_VERSION`); its users: `apps/nobion/src/main.cpp:165,730,860`, `apps/nobion/src/brand.cpp:66`, and the three `#ifdef NOBION_VERSION` sites `apps/nobion/src/publish_actions.cpp:56`, `publish_dialog.cpp:1060`, `publish_window.cpp:598` (an `#ifdef` on a removed macro goes dead silently: replace each with `nobion::build_info().version`); `setup::version()` now returns `build_info().version` (its callers `apps/elctl/src/process_log.cpp`, `apps/nobion/src/setup_support.cpp:82`, `apps/elctl/src/version_cmd.cpp:44` unchanged); afterwards `grep -rn NOBION_VERSION libs apps` finds nothing. `apps/nobion/src/main.cpp:433` and `apps/elctl/src/cli.cpp:830` (`--version`: first line `<app> <build_id()>`; elctl keeps its `profiles:` and `examples:` lines), `libs/core/src/log_hub.cpp` (`begin_session`), `libs/metrics/src/core_exporter.cpp:97` (`metrics::build_info(version)` adds label `git_sha` read from `nobion::build_info()`), `libs/experiment/src/lab/session.cpp:222` (beside `ctx.services.instrument.analyst`: set `software_version` and `software_git_sha`; every run of nobion and `elctl exp` goes through it), dashboards in `packaging/observability/grafana/dashboards/` that pin `nobion_build_info`'s labels (`libs/metrics/AGENTS.md` rule 3), `.github/workflows/release.yml` (dirty check)
- Test: `tests/core/test_log_hub.cpp`, `tests/ui/test_brand.cpp` (Qt test of `nobion::ui::build_info()`), `tests/metrics/test_core_exporter.cpp:297` (its pinned label set `{compiler,os,version}` gains `git_sha`), `tests/experiment/` (the session test that runs a simulated queue), `apps/elctl/tests/test_version_cmd.cpp` (new case driving `--version` through the CLI entry `elctl_fixture.hpp` uses)

**Interfaces:**
- Consumes: Task 1.

- [ ] **Step 1: Write the failing tests**
  - `LogHub.SessionStartNamesTheBuild`: session line contains `build=` + `nobion::build_id()`.
  - UI `TestBrand::buildInfoNamesShaAndFeatures`: `build_info()` text contains `build_info().git_sha` and a `features:` line.
  - metrics: rendered `nobion_build_info` has label `git_sha="<sha>"`.
  - experiment: a record from a simulated run has `instrument.software_git_sha == nobion::build_info().git_sha` and `software_version == build_info().version`.
  - `ElctlVersion.PrintsTheBuildId`: `elctl --version` first line `elctl <build_id()>`.
- [ ] **Step 2: Run** each filtered binary — FAIL.
- [ ] **Step 3: Implement** the replacements; nobion `--version` prints `nobion <build_id()>`. In `release.yml`, after Build: run the built `elctl --version` and fail the job if the output contains `.dirty` or `+unknown`.
- [ ] **Step 4: Run** the five filtered binaries — PASS; `ctest --preset dev-ui -R 'Brand|Version|BuildInfo|LogHub'`.
- [ ] **Step 5: Commit** `feat: the build id is in --version, About, the log, metrics and every run record`

---

## Branch 2: `feat/core-durable-writes` (Tasks 3–4)

### Task 3: `write_file_atomic`

**Files:**
- Create: `libs/core/include/nobion/core/atomic_file.hpp`, `libs/core/src/atomic_file.cpp`
- Test: `tests/core/test_atomic_file.cpp` (listed in `tests/core/CMakeLists.txt`)

**Interfaces:**
- Produces: `enum class nobion::Durability { Crash, Power }`; `Result<void> nobion::write_file_atomic(const std::filesystem::path&, std::string_view bytes, Durability = Durability::Power)`. Test seam, always compiled (no seam macro exists in the tree, and tests and apps link one `nobion_core`): `namespace nobion::testing { void fail_atomic_write_at(int step) noexcept; }` sets a `std::atomic<int>` consulted at each step (1 create temp, 2 write, 3 sync file, 4 rename, 5 sync dir; 0 = off).

- [ ] **Step 1: Write the failing tests**
  - `AtomicFile.WritesNewContent`, `AtomicFile.CreatesAFileThatWasNotThere`.
  - `AtomicFile.AFailedWriteLeavesTheOldFile`: for step in 1..4, old content `"old"`, `fail_atomic_write_at(step)`, write `"new"` → error of kind `Io`, file reads `"old"`, no `*.tmp*` left beside it.
  - `AtomicFile.AFailureAfterRenameLeavesTheNewFile`: step 5 → file reads `"new"` (error returned, content already replaced).
  - `AtomicFile.ReadOnlyDirectoryIsAnIoError` (POSIX only; skip on Windows).
- [ ] **Step 2: Run** `nobion_core_tests --gtest_filter='AtomicFile.*'` — FAIL.
- [ ] **Step 3: Implement.** Temp name beside target: `<name>.tmp.<pid>.<counter>`. POSIX: `open`/`write` loop/`fcntl(F_FULLFSYNC)` on macOS else `fsync`/`close`/`rename`/`open(dir, O_RDONLY)`+`fsync`. Windows: `CreateFileW`/`WriteFile`/`FlushFileBuffers`/`CloseHandle`/`MoveFileExW` with the retry. `Crash` skips both syncs.
- [ ] **Step 4: Measure** on the test Mac: 1000 `Power` writes of a 4 KiB file, mean and p99 ms; record here.
- [ ] **Step 5: Run** — PASS. **Commit** `feat(core): one atomic file write that survives power loss`

### Task 4: Durable state writers, and a check that keeps them so

**Files:**
- Modify (to `write_file_atomic(..., Power)`): `libs/experiment/src/executor/executor.cpp` (state write near `:694`), `libs/systems/src/extraction_line.cpp` `save_state()` (`:640`), `libs/experiment/src/persist/persister.cpp` `write_atomic`, `libs/experiment/src/lab/notifier.cpp` (`:289`), `libs/systems/src/spectrometer/field_table_store.cpp` `write_atomic`, `libs/laser/src/{calibration_store,correction_store,camera_scale,pattern}.cpp`, `libs/core/src/config/local_file.cpp`, `libs/setup/src/site.cpp`, `libs/experiment/src/model/queue_file.cpp`, `libs/experiment/src/conditionals/library.cpp`, `libs/experiment/src/factory/step_heat_template.cpp`, `libs/earthbank/src/profiles.cpp` `save_profiles`
- Create: `tools/check_atomic_writes.py`, `tools/atomic_writes_allowlist.txt`, `tools/tests/test_check_atomic_writes.py`
- Modify: `tools/githooks/pre-push` (run the check), `tests/core/CMakeLists.txt` (register the tool's unittest like `tools.quality_check`)
- Test: existing suites of each writer (`nobion_experiment_tests`, `nobion_systems_tests`, `nobion_laser_tests`, `nobion_setup_tests`, `nobion_earthbank_tests`, `nobion_core_tests`)

- [ ] **Step 1: Write the failing test** `tools/tests/test_check_atomic_writes.py`: a temp tree with `libs/x/a.cpp` containing `fs::rename(tmp, p, ec);` → exit 1 naming `a.cpp:<line>`; same tree with it in an allowlist → exit 0; `apps/x/tests/t.cpp` with `rename(` → exit 0; `atomic_file.cpp` itself never reported.
- [ ] **Step 2: Run** `python3 -m unittest tools/tests/test_check_atomic_writes.py` — FAIL.
- [ ] **Step 3: Implement** the script (scans `.cpp` under `libs/` and `apps/` for `rename(`, skipping any path with a `tests` directory component; allowlist lines are file paths), then move each writer above. Allowlist: `libs/earthbank/src/journal.cpp`, `libs/earthbank/src/uploader.cpp`, `libs/earthbank/src/ooxml.cpp`, `libs/vision/src/png.cpp`, `libs/transport/src/endpoints.cpp`, `libs/processing/src/options.cpp`, `libs/processing/src/isotope_classifier.cpp`, `apps/nobion/src/script_editor_window.cpp`, `apps/elctl/src/flux.cpp`.
- [ ] **Step 4: Line state cost.** With Task 3's number: if a `Power` write costs over 20 ms, `ExtractionLine::save_state()` coalesces (latest state, one write per 250 ms on a `Scheduler::after` job, flushed in `stop()`), except a manual-valve change, which writes at once. Test either way: `ExtractionLine.StateSurvivesStop` (actuate, stop, reload: state matches), plus `ExtractionLine.ManualValveStateIsWrittenAtOnce` if coalescing.
- [ ] **Step 5: Run** `python3 tools/check_atomic_writes.py` (exit 0) and the six suites — PASS. **Commit** `fix: state the process reads back after a restart survives power loss`

---

## Branch 3: `feat/core-crash-capture` (Tasks 5–9)

### Task 5: POSIX crash handler writes build, thread, backtrace, modules

**Files:**
- Modify: `libs/core/src/log_hub.cpp` (handlers, `retarget_crash_output`, `LogHub::create` makes `<dir>/crash`), `libs/core/include/nobion/core/log_hub.hpp` (doc comment)
- Create: `libs/core/include/nobion/core/signal_stack.hpp`, `libs/core/src/signal_stack.cpp`, `libs/core/src/module_map.{hpp,cpp}` (internal)
- Modify (call `install_signal_stack()` first thing): `tests/support/virtual_time.hpp` `Crew`, `libs/transport/src/queued_transport.cpp` `run_worker`, `libs/core/src/scheduler.cpp` workers, `libs/experiment/src/executor/executor.cpp` thread entry, `libs/devices/src/spectrometer/ngx_link.cpp` reader, `LogHub` writer/flusher threads (the hang monitor and recorder-trigger threads, Tasks 12–13, call it too)
- Create: `tests/core/crash_helper.cpp`, `tests/core/test_crash_capture.cpp`
- Modify: `tests/core/CMakeLists.txt`: `add_executable(nobion_crash_helper crash_helper.cpp)` linking `nobion::core`; the test target gets `NOBION_CRASH_HELPER="$<TARGET_FILE:nobion_crash_helper>"` and `add_dependencies(nobion_core_tests nobion_crash_helper)`; `test_crash_capture.cpp` added to its sources

**Interfaces:**
- Produces: `void nobion::install_signal_stack() noexcept` (idempotent per thread; 64 KiB, freed at thread exit via a `thread_local` owner); internal `ModuleMap` with `refresh()` (outside signals) and `write_to(int fd) noexcept` (signal-safe); `LogHub::crash_file_path() const` (for tests).

- [ ] **Step 1: Write the failing tests.** `crash_helper <log dir> <mode>` creates a hub on `<log dir>`, calls `install_crash_handlers()`, logs `ring marker` at info, then acts by mode: `sleep` (blocks until killed; used by Task 7), or dies by `segv`, `abort`, `bus` (POSIX `raise(SIGBUS)`), `fpe`, `ill`, `trap`, `overflow-main`, `overflow-thread` (thread with `install_signal_stack()`), `raw-thread-segv` (Review Focus 1), `hardened` (`std::vector<int> v(1); v[5]`, only when `has_feature("hardened_stdlib")`). Tests `CrashCapture.<Mode>WritesACrashFile` spawn it (`posix_spawn`), wait, and assert the file `<log dir>/crash/crash_helper-<pid>.txt` holds: `fatal signal <N>`, `build <build_id()>`, a `thread ` line, ≥ 3 lines matching `^0x[0-9a-f]+$`, a line `module <path> 0x<load hex> 0x<size hex>` whose path ends in `crash_helper` (the exact form Task 8 parses; on Linux the main executable, which `dl_iterate_phdr` reports with an empty name, is named from `readlink("/proc/self/exe")` at install); the process died by that signal (`WIFSIGNALED`, `WTERMSIG`). Skipped on Windows (Task 6).
- [ ] **Step 2: Run** `nobion_core_tests --gtest_filter='CrashCapture.*'` — FAIL.
- [ ] **Step 3: Implement** per spec 3.2: add `SIGTRAP`; `SA_ONSTACK`; `alarm(5)` first; preformat `build` line and crash path at install, re-set in `retarget_crash_output` and `begin_session`; `backtrace()` once at install; thread line from `log_context` `t_tag` (`data()`/`size()` of the pointed-to string, `?` if null); module map (macOS `_dyld_register_func_for_add_image` into static 512-entry table; Linux `dl_iterate_phdr` at install and from the flusher when `dlpi_adds` changes, double-buffered with an atomic index). Crash file opened in the handler; at hub start remove crash files beyond the newest 20.
- [ ] **Step 4: Run** — PASS. Also under `-DNOBION_SANITIZE=address,undefined` build: tests skip themselves under ASan, which owns the signals: `defined(__SANITIZE_ADDRESS__)` (gcc) or `__has_feature(address_sanitizer)` inside `#if defined(__has_feature)` (clang); `GTEST_SKIP` says why.
- [ ] **Step 5: Commit** `feat(core): a crash leaves a file with its build, thread, backtrace and modules`

### Task 6: Windows crash file and minidump

**Files:**
- Modify: `libs/core/src/log_hub.cpp` (Windows filter, crash thread)
- Test: `tests/core/test_crash_capture.cpp` (Windows branch)

- [ ] **Step 1: Write the failing test** `CrashCapture.WindowsWritesTextAndDump`: helper mode `segv` (null write) and `overflow-main`; parent finds `crash_helper-<pid>.txt` with `fatal exception 0x` and `build`, and `.dmp` that `MiniDumpReadDumpStream(..., ThreadListStream, ...)` reads.
- [ ] **Step 2: Run** on a Windows machine if one is at hand (expect FAIL). Otherwise its first run is the Windows job of the next `develop`→`main` pull request (Global Constraints); `compiler_sweep.py` cannot check `_WIN32` code, so read the MSVC warning list in root `AGENTS.md` before landing.
- [ ] **Step 3: Implement** per spec 3.2 Windows: crash thread started at install, dump handle opened at install (re-opened on retarget), filter writes text then signals and waits ≤ 10 s.
- [ ] **Step 4: Run** — PASS on Windows.
- [ ] **Step 5: Commit** `feat(core): a crash on Windows leaves a minidump`

### Task 7: Unclean exit detected at the next start

**Files:**
- Modify: `libs/core/include/nobion/core/log_hub.hpp`, `libs/core/src/log_hub.cpp` (sentinel, `previous_session()`), `apps/nobion/src/main.cpp` (dialog after the main window shows), `apps/elctl/src/process_log.cpp` (`open_process_log`, `:88`: the stderr line once the hub is open; `line.cpp:79` and `exp.cpp:320` open hubs too and print it the same way)
- Create: `apps/nobion/src/unclean_exit_dialog.{hpp,cpp}`
- Test: `tests/core/test_log_hub.cpp`, `tests/ui/test_unclean_exit_dialog.cpp`; the helper's `sleep` mode is Task 5's

**Interfaces:**
- Produces: `struct nobion::PreviousSession { std::string build_id; std::string started; std::optional<std::filesystem::path> crash_file; }`; `std::optional<PreviousSession> LogHub::previous_session() const`; `UncleanExitDialog(const PreviousSession&, QWidget*)` with signal `collectRequested()` (Task 19 connects it).

- [ ] **Step 1: Write the failing tests**
  - `LogHub.ACleanEndLeavesNoSentinel`.
  - `LogHub.AKilledSessionIsReportedOnce`: spawn `crash_helper <dir> segv`; new hub on `<dir>` → `previous_session()` has the helper's build and `crash_file` set; an error record `ended without a clean exit`; a third hub → `nullopt`.
  - `LogHub.ALiveSecondProcessIsNeverReportedUnclean`: helper mode `sleep` holding its own hub with stem `crash_helper`; a hub in the test with stem `crash_helper` too (gets `crash_helper-<pid>`) → `nullopt` while the helper lives.
  - UI `uncleanExitDialogOffersCollect`: dialog text contains the build id; clicking **Collect Diagnostics…** emits `collectRequested`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** per spec 3.3 (`flock(LOCK_EX|LOCK_NB)`; Windows `LockFileEx` with `LOCKFILE_FAIL_IMMEDIATELY`).
- [ ] **Step 4: Run** `nobion_core_tests --gtest_filter='LogHub.*'`, `nobion_ui_test_unclean_exit_dialog` — PASS.
- [ ] **Step 5: Commit** `feat: the next start says when the last session did not end cleanly`

### Task 8: Symbols kept per release, and a symbolizer

**Files:**
- Modify: `.github/workflows/release.yml` (`RelWithDebInfo`; split step before `cpack`; upload symbols asset; release-config test step; symbolize check), `CMakeLists.txt` or `cmake/NobionInstall.cmake` (`file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/symbol_files.txt ...)` over our executables and shared libraries; `$<TARGET_PDB_FILE>` on MSVC)
- Create: `tools/split_symbols.py`, `tools/symbolize.py`, `tools/tests/test_symbolize.py`

**Interfaces:**
- Consumes: Task 5's crash file format (address lines `0x...`, module lines `module <name> <load addr hex> <size hex>`; Task 5 fixes that exact line form, this task parses it).

- [ ] **Step 1: Write the failing test** `tools/tests/test_symbolize.py`: given a crash text and a fake `atos`/`addr2line` on `PATH` (a script that echoes its arguments), `symbolize.py` calls it once per frame with the right module and offset (`-l <load>` on macOS; `0x<addr - load>` on Linux) and prints `#n function file:line`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** both tools (spec 3.1) and the workflow steps: build tests for `nobion_arar_tests nobion_reduction_tests nobion_processing_tests nobion_core_tests nobion_vision_tests` in the release config and run them before packaging; after packaging, run `nobion_crash_helper` (built in the release job) mode `segv`, then `symbolize.py` against the split symbols, and fail unless a frame names `crash_helper.cpp`.
- [ ] **Step 4: Measure** `-O2` (RelWithDebInfo) against `-O3` (Release): `ctest --preset dev` wall time is not affected by this (Debug); measure instead a release-config build of `elctl` running the example queue at `--sim-speed max`, three runs each; record here. Over 3%: stop and ask the owner.
- [ ] **Step 5: Run** the unittest — PASS; run the workflow on a branch with `workflow_dispatch` and an rc tag input if the workflow allows; else the first `develop`→`main` PR. **Commit** `ci: releases keep their symbols, and a crash file can be symbolized`

### Task 9: Hardened standard library

**Files:**
- Create: `cmake/NobionHardening.cmake`
- Modify: `CMakeLists.txt` (include), `cmake/NobionWarnings.cmake` (`nobion_set_warnings(<target>)`, the per-target helper with PRIVATE options, also applies the hardening definitions; check that the `nobion` and `elctl` targets call it, and add the call where one does not)

- [ ] **Step 1: Write the failing test** `BuildInfo.HardenedStdlibOnWhereSupported`: on libstdc++ or libc++ ≥ 17 and not sanitized, `has_feature("hardened_stdlib")` is true; on MSVC false. And Task 5's `CrashCapture.HardenedWritesACrashFile` now runs instead of skipping.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** spec 4 (configure check for `_LIBCPP_VERSION` / `__GLIBCXX__`; skip when `NOBION_SANITIZE` set; set `NOBION_HARDENED_STDLIB` for Task 1's features).
- [ ] **Step 4: Measure** before/after: `ctest --preset dev` wall and the example queue at `--sim-speed max`; record here. Over 3% on either: stop and ask the owner. Then full `ctest --preset dev-ui` — PASS (a library check firing in our own tests is a bug to fix, not a reason to drop the macro).
- [ ] **Step 5: Commit** `build: the standard library checks its own preconditions in every build`

---

## Branch 4: `feat/core-flight-recorder` (Tasks 10–13)

### Task 10: Recorder level beside the file level

**Files:**
- Modify: `libs/core/include/nobion/core/logger.hpp`, `libs/core/src/logger.cpp`, `libs/core/src/log_hub.cpp` (recorder rules, `resolve_locked` twin, `hub_resolve_level` returns both), `libs/core/include/nobion/core/config/logging_config.hpp` and its parser (`[recorder]`), `configs/examples/logging.toml` (commented `[recorder]` block)
- Test: `tests/core/test_logger.cpp`, `tests/core/test_logging_file.cpp`

**Interfaces:**
- Produces: `config::RecorderConfig{bool enabled = true; std::size_t size_mb = 32; LogLevel default_level = LogLevel::Debug; std::vector<std::pair<std::string, LogLevel>> levels{{"*.wire", LogLevel::Trace}};}` in `LoggingConfig::recorder`; `LogHub::set_recorder_levels(LogLevel default_level, const std::vector<std::pair<std::string, LogLevel>>&)`; `Logger::recorder_level() const noexcept`; `Logger::enabled(l)` true when `l >= min(level(), recorder_level())`.

- [ ] **Step 1: Write the failing tests**
  - `LoggingFile.RecorderTableIsRead` (values above, `size_mb` 3 and 600 refused with the key's `file:line`).
  - `Logger.LevelStaysTheFileLevel`: file rules `*.wire=info`, recorder `*.wire=trace` → `level()==Info`, `recorder_level()==Trace`, `enabled(Trace)`.
  - `Logger.ExplicitLevelLeavesTheRecorderAlone`: `set_level(Error)` → `recorder_level()` unchanged.
  - `LogHub.ARecordBelowTheFileLevelIsNotInTheFile`: trace on `x.wire` → not in file, not on bus, not on stderr.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Second packed atomic in `Logger` (`recorder_state_`), same epoch scheme; `log()` routes by spec 5.1. (Ring arrives in Task 11; here the record below file level is formatted and dropped behind a hub call `record_only(...)` that Task 11 fills.)
- [ ] **Step 4: Run** `nobion_core_tests --gtest_filter='Logger.*:LogHub.*:LoggingFile.*'` — PASS.
- [ ] **Step 5: Commit** `feat(core): loggers carry a recorder level beside the file level`

### Task 11: The ring, snapshots and dumps

**Files:**
- Create: `libs/core/src/byte_ring.{hpp,cpp}` (internal)
- Modify: `libs/core/src/log_hub.cpp`, `libs/core/include/nobion/core/log_hub.hpp` (crash handler writes ring spans: POSIX and Windows paths of Tasks 5–6)
- Test: `tests/core/test_byte_ring.cpp`, `tests/core/test_log_hub.cpp`, `tests/core/test_crash_capture.cpp`

**Interfaces:**
- Produces: `std::string LogHub::recorder_snapshot() const`; `Result<std::filesystem::path> LogHub::dump_recorder(std::string_view reason)`.

- [ ] **Step 1: Write the failing tests**
  - `ByteRing.KeepsWholeNewestLines`, `ByteRing.NeverExceedsItsSize`, `ByteRing.SpansAreReadableWithoutTheLock` (two spans concatenate to the snapshot).
  - `LogHub.SnapshotHoldsWireTraceNotInTheFile`.
  - `LogHub.DumpWritesReasonAndBuildFirst`: first line `flight recorder dump: reason=manual build=<build_id()>`.
  - `LogHub.SameReasonWithinAMinuteWritesNothing`, `LogHub.AnyReasonWithinTenSecondsWritesNothing` (inject the rate-limit clock: `LogHub` takes a `std::function<std::chrono::steady_clock::time_point()>` for limits through a test-only setter).
  - `LogHub.KeepsTheNewestTwentyDumps`.
  - `LogHub.ADumpToAReadOnlyDirectoryFailsCleanly` (Review Focus 4): `Io` error, no file, process continues.
  - `CrashCapture.CrashFileHoldsTheRing`: helper's `ring marker` line is in its crash file.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** spec 5.2–5.4 (allocate once; `std::atomic<std::uint64_t>` head/tail; producer thread-local format buffer; mutex only for copy).
- [ ] **Step 4: Run** — PASS. TSan build: `nobion_core_tests --gtest_filter='ByteRing.*:LogHub.*'`.
- [ ] **Step 5: Commit** `feat(core): the log hub keeps the last minutes in memory and writes them out on request`

### Task 12: Triggers, app wiring, measurement

**Files:**
- Create: `libs/core/include/nobion/core/flight_recorder_triggers.hpp`, `libs/core/src/flight_recorder_triggers.cpp`, `libs/experiment/include/nobion/experiment/flight_recorder_wiring.hpp`, `libs/experiment/src/flight_recorder_wiring.cpp`
- Modify: `apps/nobion/src/main.cpp`, `apps/elctl/src/line.cpp` (after its hub and `bus_` exist, `:79`) and `apps/elctl/src/exp.cpp` (`exp run`, after its hub `:320` and bus `:460`), `apps/nobion/src/brand.cpp` `add_help_menu` (Help > Save Flight Recorder)
- Test: `tests/core/test_flight_recorder_triggers.cpp`, `tests/experiment/test_flight_recorder_wiring.cpp`

**Interfaces:**
- Consumes: Task 11.
- Produces: `struct nobion::IDumpRequester { virtual ~IDumpRequester() = default; virtual void request(std::string reason) = 0; };` `class FlightRecorderTriggers final : public IDumpRequester` with `FlightRecorderTriggers(LogHub&, SignalBus&, std::function<Result<std::filesystem::path>(std::string_view)> dump = {})` (empty `dump`: `hub.dump_recorder`; tests pass one that waits on a latch); `[[nodiscard]] std::vector<SignalBus::Subscription> nobion::experiment::wire_flight_recorder(SignalBus&, FlightRecorderTriggers&)` (subscribes `RunFinished` failed → `run-failed-<run id>`, `QueueEnded` unsafe → `unsafe`). `DeviceHealthChanged` subscription added in Task 14.

- [ ] **Step 1: Write the failing tests**
  - `FlightRecorderTriggers.ACriticalAlarmDumps` (reason `alarm-<source>`); `...AWarningAlarmDoesNot`.
  - `FlightRecorderTriggers.RequestNeverBlocksThePublisher`: `dump` waits on a latch; a critical `Alarm` published returns while the latch is closed (bound 5 s); latch opened, the dump happens.
  - `FlightRecorderTriggers.AStormKeepsItsFirstDump` (Review Focus 5): 30 critical alarms in 1 s → one dump, its reason the first alarm's.
  - `FlightRecorderWiring.AFailedRunDumps`, `...AnUnsafeQueueDumps`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement**; wire in both apps after hub and bus; Help menu action calls `dump_recorder("manual")` and shows the path in the status bar.
- [ ] **Step 4: Measure** (spec 14): example lab `nobion --sim`, 5 minutes, `*.wire` recorded vs recorder disabled; CPU % (`ps -o %cpu` sampled each second) and records/s; record here. Over 2% added: make `.wire` loggers format only into the ring (spec 5.1) and measure again.

  Measured 2026-11-07, Apple M5 Pro (18 cores), `RelWithDebInfo` `Nobion.app`,
  `QT_QPA_PLATFORM=offscreen`, `nobion --sim` on a copy of `configs/examples`
  (line only, no spectrometer, no queue), log folder and `HOME` in a temporary
  directory; 20 s start-up left out, then 300 samples one second apart:

  | | `ps -o %cpu` mean (max) | CPU time over the 300 s | of one core |
  |---|---|---|---|
  | recorder on, `*.wire = "trace"` (default) | 0.699% (2.2) | 2.81 s | 0.94% |
  | `[logging.recorder] enabled = false` | 0.693% (2.4) | 2.75 s | 0.92% |

  Records: a third run of 60 s with the file at `debug` and `*.wire = "trace"`
  wrote 5015 lines in 80 s (start-up included), 4015 of them `*.wire`: about
  50 wire records a second and 62 records a second in all. Added by the
  recorder: 0.06 s of CPU in 300 s (0.02 percentage points of one core), within
  run-to-run noise; at most 4 µs a record. Under 2%: the `.wire` path is left
  as Task 11 built it (a record below the file level is formatted into the ring
  only), `enabled()` keeps its two atomic loads, and spec 5.1 says so (no cached
  `gate`).

  Re-measured after Task 16 (2026-11-07, same machine): spectrometer
  transports now log to `<transport>.wire`. The `sim_*` spectrometer drivers
  never touch a transport, so the load is the example Qtegra spectrometer
  (`spectrometer.qtegra.toml`) served by `elctl sim serve` and driven by
  `elctl --sim-endpoints ... exp run queues/experiment.toml` (real time,
  `RelWithDebInfo`, a copy of `configs/examples`, log folder and `HOME` in a
  temporary directory; `nobion --sim` is no use here, its spectrometer is idle
  without a run started by hand). 20 s left out, then 300 one-second `ps -o %cpu`
  samples of the client, which holds the recorder:

  | | `ps -o %cpu` mean (max) | CPU time over the 300 s | of one core |
  |---|---|---|---|
  | recorder on, `*.wire = "trace"` (default) | 0.064% (0.5) | 1.01 s | 0.34% |
  | `[logging.recorder] enabled = false` | 0.069% (1.4) | 0.97 s | 0.32% |

  Records: a run with the file at `debug` and `*.wire = "trace"` wrote 10331
  lines in 324 s, 9726 of them `*.wire`, 1068 of those `qtegra.wire`: about 30
  wire records a second in all, 3 a second from the spectrometer. Added by the
  recorder: 0.04 s of CPU in 300 s (0.01 percentage points of one core), within
  noise; under 2%, nothing changed.
- [ ] **Step 5: Run** suites — PASS; TSan run of the two new suites. **Commit** `feat: alarms, failed runs and the operator write out the flight recorder`

### Task 13: Hang monitor

**Files:**
- Create: `libs/core/include/nobion/core/hang_monitor.hpp`, `libs/core/src/hang_monitor.cpp`
- Modify: `libs/transport/src/queued_transport.cpp` (`TransportOptions` or ctor gains `HangMonitor*`; pulse at call start, disarm at end), `libs/transport/src/factory.cpp` (`TransportContext::hang_monitor`), `apps/nobion/src/main.cpp` (monitor, `ui.main` QTimer, scheduler beat when the clock is a `SteadyClock`), `apps/elctl/src/exp.cpp` (monitor for `exp run` on a `SteadyClock`, beside the triggers of Task 12), `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` §4.4 (add hang monitor, recorder triggers worker), `docs/installation_runbook.md` (per-OS stack commands)
- Test: `tests/core/test_hang_monitor.cpp` (listed in `tests/core/CMakeLists.txt`), `tests/transport/test_queued_transport.cpp`

**Interfaces:**
- Consumes: `IDumpRequester` (Task 12).
- Produces: spec 6 `HangMonitor`, constructor `HangMonitor(LogHub&, IDumpRequester&, SignalBus&, Options = {})` (the spec says `FlightRecorderTriggers&`: amend spec §6 in this commit), `HangMonitor::Beat` (`pulse()`, `disarm()`), `Beat HangMonitor::beat(std::string name, std::chrono::milliseconds limit)`; `TransportOptions::hang_limit` (`std::optional<std::chrono::milliseconds>`, in `libs/transport/include/nobion/transport/transport.hpp`; unset: the spec's formula) and `TransportContext::hang_monitor` (`HangMonitor*`).

- [ ] **Step 1: Write the failing tests** (real time, short limits; upper bounds ≥ 5 s per root `AGENTS.md`)
  - `HangMonitor.ASilentBeatReportsOnce`: limit 200 ms, poll 50 ms, fake `IDumpRequester`; wait (bound 5 s) until it has one `request("hang-b")`; wait a further 1 s: still exactly one request, one `hang: b silent` record, one `Alarm{source="hang"}`.
  - `HangMonitor.APulseRearms`: after the first report, pulse, wait (bound 5 s) for a second.
  - `HangMonitor.ADisarmedBeatNeverReports`: a disarmed beat and an armed control beat, both limit 50 ms; once the control has reported (bound 5 s), the disarmed one has not.
  - `QueuedTransport.AStuckCallIsReported`: `hang_limit` 200 ms; a sim hook blocks on a latch; wait (bound 5 s) for the report naming `transport.<name>`; open the latch; the call completes.
  - `QueuedTransport.AnIdleTransportIsNotWatched`: `hang_limit` 50 ms, no calls, an armed control beat (limit 50 ms) on the same monitor; once the control has reported (bound 5 s), the transport has not.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** spec 6; the monitor thread calls `install_signal_stack()` first. The scheduler beat and transport beats are registered only when `dynamic_cast<const VirtualClock*>(&clock) == nullptr`.
- [ ] **Step 4: Run** — PASS; TSan run of both suites; `ctest --preset dev -R 'Outage|LinkOutage|VirtualClock'` to show the clock rules still hold.
- [ ] **Step 5: Commit** `feat: a hang monitor reports a silent UI, scheduler or transport call`

---

## Branch 5: `feat/devices-health` (Tasks 14–16)

### Task 14: `DeviceHealthChanged` on the bus, logged and exported

**Spec amendment (same branch):** spec section 7 says `DeviceOptions` gains the bus. Drivers build `DeviceOptions` themselves (26 drivers), so instead the registry hands the bus over after construction: `DriverContext` gains `SignalBus* bus = nullptr`; the other overload `create(const config::DriverConfig&, Transport&, const Clock*, Link*)` (used by `extraction_line.cpp:175` and `apps/elctl/src/line.cpp:121`; it builds its own `DriverContext` at `driver_registry.cpp:173`) gains a trailing `SignalBus* bus = nullptr`; both call `device->set_health_bus(bus)` after the factory. `DeviceOptions` unchanged. `enum class DeviceState` and its `to_string` move from `libs/devices/include/nobion/devices/device.hpp:20` to `libs/core/include/nobion/core/device_state.hpp` (namespace `nobion`; `device.hpp` includes it, so its users compile unchanged), because the event lives in `libs/core` and core does not depend on devices. Metrics: gauges `nobion_device_state` and `nobion_device_consecutive_failures` from the event (a failures counter cannot be driven by state changes alone); series are removed the way the exporter removes transport series today, and if it never removes those, neither does this. Edit spec §7 to say all this in this task's commit.

**Files:**
- Create: `libs/core/include/nobion/core/device_state.hpp`, `libs/core/src/device_state.cpp`
- Modify: `libs/core/include/nobion/core/events.hpp` (`DeviceHealthChanged`), `libs/devices/include/nobion/devices/device.hpp`, `libs/devices/src/device.cpp` (`record_failure(const Error&)`; state before/after; publish on change), `libs/devices/include/nobion/devices/driver_registry.hpp` + `.cpp` (`DriverContext::bus`), call sites `libs/systems/src/extraction_line.cpp:175`, `libs/systems/src/spectrometer/assembler.cpp:184`, `apps/elctl/src/line.cpp:121`, `libs/systems` event recorder (`event_log.hpp`), `libs/metrics/src/core_exporter.cpp` (gauges created at zero/unknown, `libs/metrics/AGENTS.md` rule 1), a panel for both gauges in `packaging/observability/grafana/dashboards/instrument-health.json` (rule 3; `MetricsPackaging` tests fail without it), `libs/core/src/flight_recorder_triggers.cpp` (subscribe), spec §7
- Test: `tests/devices/test_device.cpp` (fixture `Plain`: new cases there), `tests/systems/test_event_log.cpp`, `tests/metrics/test_core_exporter.cpp`, `tests/core/test_flight_recorder_triggers.cpp`

**Interfaces:**
- Produces: `struct DeviceHealthChanged { std::string device, transport; DeviceState from, to; std::uint64_t consecutive_failures; std::optional<ErrorKind> last_error_kind; std::string last_error_text; TimePoint ts; };` `void Device::set_health_bus(SignalBus*) noexcept`.

- [ ] **Step 1: Write the failing tests**
  - `Plain.FaultAfterPublishesOnceToFaulted`: fake failing `fault_after` times → events `Unknown→Degraded` (first failure, if that is how `state` derives today; read `Device::health()` first and assert the real sequence), then one `→Faulted`; further failures publish nothing; `last_error_text` is the last `Error::what`.
  - `Plain.RecoveryPublishesOk`.
  - `Plain.NoBusPublishesNothing`.
  - `SystemsEventLog.FaultedIsAWarning`.
  - `CoreExporter.DeviceStateGauge` (`nobion_device_state{device="ig1",transport="gauges"} 3`), `nobion_device_consecutive_failures{device="ig1"} 3`.
  - `FlightRecorderTriggers.AFaultedDeviceDumps`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Handlers never call into a device (comment on the event).
- [ ] **Step 4: Run** the four suites and `nobion_systems_tests` — PASS.
- [ ] **Step 5: Commit** `feat(devices): a device going bad is published, logged and exported`

### Task 15: HealthBar shows devices

**Files:**
- Modify: `apps/nobion/src/core_bridge.{hpp,cpp}` (relay `DeviceHealthChanged`), `apps/nobion/src/health_bar.{hpp,cpp}`
- Test: new `tests/ui/test_health_bar_devices.cpp` (globbed into `nobion_ui_test_health_bar_devices`)

**Interfaces:**
- Produces: `void HealthBar::update_device(const DeviceHealthChanged&)`; `Status HealthBar::chip_status(const QString& transport) const` (for tests).

- [ ] **Step 1: Write the failing tests** `transportUpWithAFaultedDeviceIsAmber`, `tooltipListsEachDevice` (state, failures, last error text, last ok "ago" from the injected `now`), `recoveredDeviceClearsAmber`.
- [ ] **Step 2: Run** `nobion_ui_test_health_bar_devices` — FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `feat(ui): the health bar names the device that is failing`

### Task 16: Spectrometer transports log their bytes

**Files:**
- Modify: `libs/systems/include/nobion/systems/spectrometer/spectrometer.hpp:149` (`std::shared_ptr<LogHub> log_hub`), `libs/systems/src/spectrometer/assembler.cpp` (`tctx.log_hub`), every `SpectrometerContext{...}` construction: both apps, `libs/setup/src/connection.cpp:69` (no hub there: leave null), and the test fixtures `tests/ui/spectrometer_fixture.hpp:65`, `tests/ui/test_plot_build.cpp:43`, `tests/ui/experiment_fixture.hpp:91` (designated initializers must name every field or clang 18 warns: root `AGENTS.md`)
- Test: `tests/systems/test_spectrometer_assembler.cpp` (or where the assembler is tested)

- [ ] **Step 1: Write the failing test** `SpectrometerAssembler.TransportsLogWireBytes`: hub with `*.wire=trace`, assemble the sim-integrated example over a sim transport, one exchange → hub file (or snapshot, after Task 11) has a `<transport>.wire: tx` line.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `nobion_systems_tests` — PASS.
- [ ] **Step 5: Commit** `fix(spectrometer): spectrometer transports log their bytes like line transports`

---

## Branch 6: `feat/setup-support-bundle` (Tasks 17–19)

### Task 17: One redaction rule

**Files:**
- Create: `libs/core/include/nobion/core/redact.hpp`, `libs/core/src/redact.cpp`
- Modify: `libs/core/src/log_summary.cpp` (drop private `is_secret_key`, call `redact::is_secret_key`)
- Test: `tests/core/test_redact.cpp`, `tests/core/test_log_summary.cpp`

**Interfaces:**
- Produces: `namespace nobion::redact { bool is_secret_key(std::string_view) noexcept; std::string in_text(std::string_view); std::string in_toml(std::string_view); inline constexpr std::string_view kMask = "***"; }`

- [ ] **Step 1: Write the failing tests**
  - `Redact.KeysAreSecretEqualOrBySuffix`: true for `password`, `DB_Password`, `api_key`, `smtp-passwd`, `credential`, `brevo_key`; false for `keyboard`, `monkey`, `tokenizer`.
  - `Redact.UrlPasswordsAreMasked`: `postgres://u:pw@h/db` → `postgres://u:***@h/db`; `https://h/x` unchanged; two URLs in one line both masked.
  - `Redact.TomlByKeyAndByValue`: `password = "x"` → `password = "***"`; `database = "postgres://u:pw@h/db"` → masked by value; comments and other keys unchanged; inline tables handled.
  - existing `LogSummary.*` still PASS.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** (`in_toml` is line-based: `key = value` where key matches, value replaced by `"***"`; then `in_text` over the result).
- [ ] **Step 4: Run** `nobion_core_tests --gtest_filter='Redact.*:LogSummary.*'` — PASS.
- [ ] **Step 5: Commit** `refactor(core): one redaction rule for logs and support bundles`

### Task 18: `write_support_bundle`

**Files:**
- Create: `libs/setup/include/nobion/setup/support_bundle.hpp`, `libs/setup/src/support_bundle.cpp`
- Modify: `libs/setup/CMakeLists.txt` (`PRIVATE miniz::miniz`), doctor entry used without `--probe` (`libs/setup/include/nobion/setup/doctor.hpp`)
- Test: `tests/setup/test_support_bundle.cpp` (added to `NOBION_SETUP_TESTS` in `tests/setup/CMakeLists.txt`)

**Interfaces:**
- Consumes: Task 11 (`dump_recorder`), Task 17, Task 1.
- Produces: spec 8 `BundleOptions`, `Result<std::filesystem::path> write_support_bundle(const setup::SiteInstall*, const BundleOptions&)` (the spec says `setup::Install`; the type is `SiteInstall`, `libs/setup/include/nobion/setup/site.hpp:33`: amend spec §8 in this commit); the hub for the fresh dump is passed as `std::shared_ptr<LogHub> hub` in `BundleOptions` (null: no fresh dump).

- [ ] **Step 1: Write the failing tests** (example install made in a temp dir with `elctl init` machinery the setup tests already use)
  - `SupportBundle.HoldsEverySection`: zip entries include `README.txt`, `build.json`, `logs/`, `config/install.toml`, `config/EDITED.txt`, `state/`, `doctor.txt`.
  - `SupportBundle.EditedListsChangedFiles`: edit one config file → listed; an unedited one not.
  - `SupportBundle.NoSecretSurvives` (Review Focus 3): seed `.nobion/credentials.toml` (`password = "s3cret-A"`), `site.toml` `database = "postgres://u:s3cret-B@h/db"`, `notifications.toml` `brevo_key = "s3cret-C"`, a log line containing `postgres://u:s3cret-D@h/db` → unzip all, none of the four strings in any entry; `credentials.toml` absent.
  - `SupportBundle.KeepsUnderMaxBytes`: 5 MiB of rotated logs, `max_bytes = 1 MiB` → zip ≤ 1 MiB, newest log kept, README names what was dropped.
  - `SupportBundle.LiveStateIsIncludedWhenGiven`.
- [ ] **Step 2: Run** `nobion_setup_tests --gtest_filter='SupportBundle.*'` — FAIL.
- [ ] **Step 3: Implement.** `doctor.txt` holds `doctor(const SiteInstall&)`'s checks, one a line; with a null install it says `no install`. `config/install.toml` is copied from `<root>/.nobion/install.toml` (`record_path`); `EDITED.txt` compares against `InstallRecord::files`.
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `feat(setup): a support bundle a lab can send back`

### Task 19: `elctl support-bundle` and Help > Collect Diagnostics

**Files:**
- Modify: `apps/elctl/src/cli.cpp` (setup command block near `:853`, usage text), new `apps/elctl/src/support_bundle_cmd.{hpp,cpp}`, `apps/nobion/src/brand.cpp` `add_help_menu` (Collect Diagnostics…), new `apps/nobion/src/collect_diagnostics.{hpp,cpp}` (main-thread `live` snapshot from `CoreBridge`/`ExperimentBridge`, worker thread for the write, progress, reveal), `apps/nobion/src/main.cpp` (connect `UncleanExitDialog::collectRequested`), `docs/user/10-troubleshooting.md` (how to collect and send a bundle)
- Test: `apps/elctl/tests/test_support_bundle_cmd.cpp` (added in `apps/elctl/tests/CMakeLists.txt`), `tests/ui/test_collect_diagnostics.cpp`

- [ ] **Step 1: Write the failing tests**
  - `ElctlSupportBundle.WritesTheZipAndPrintsItsPath`, `...SinceAndNoTracesAreHonoured`.
  - UI `collectDiagnosticsWritesAZipWithLiveState`: action triggered offscreen, wait for done, zip has `live.json` with a `transports` key.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `nobion_elctl_tests --gtest_filter='ElctlSupportBundle.*'`, `nobion_ui_test_collect_diagnostics` — PASS.
- [ ] **Step 5: Commit** `feat: collect diagnostics from elctl, the Help menu or the unclean-exit dialog`

---

## Branch 7: `feat/persistence-migration-backup` (Task 20)

### Task 20: Back up a store before migrating it

**Spec amendment (same branch):** spec section 11 names a `MigrationReport{backup}`; `open_store` returns only the store, so instead `StoreConfig` gains `std::function<void(const std::filesystem::path&)> on_backup` called with the backup's path. Edit spec §11 in this task's commit.

**Files:**
- Modify: `libs/persistence/include/nobion/persistence/store.hpp` (`StoreConfig::backup_before_migrate = true`, `on_backup`), `libs/persistence/src/migrate.{hpp,cpp}` (`migrate(Db&, bool apply, std::optional<int> up_to, const BackupPolicy&)`), `libs/persistence/src/store.cpp:931`, `apps/elctl/src/db.cpp` (`migrate`: print backup path; PostgreSQL refusal and `--backed-up`; usage text), spec §11
- Test: `tests/persistence/test_schema.cpp` (or the suite that runs `migrate`), `apps/elctl/tests/test_db_cmd.cpp`

**Interfaces:**
- Produces: `struct BackupPolicy { bool enabled; std::function<void(const std::filesystem::path&)> on_backup; };` (internal to `libs/persistence`).

- [ ] **Step 1: Write the failing tests**
  - `Migrate.APendingMigrationOnAFileStoreIsBackedUpFirst`: store at migration N−1 (apply with `up_to`), then open with migrate → `on_backup` called once; backup file opens with `schema_status()` max version N−1; name matches `nobion.pre-<N-1>-to-<N>-*.sqlite`.
  - `Migrate.AFreshStoreIsNotBackedUp`, `Migrate.AnUpToDateStoreIsNotBackedUp`, `Migrate.AnInMemoryStoreIsNotBackedUp`.
  - `Migrate.AFailedBackupRefusesTheMigration` (backup target dir read-only) → `Io`, schema still N−1.
  - `Migrate.APathWithAQuoteIsBackedUp` (db dir name contains `'`).
  - `ElctlDb.MigratePrintsTheBackup`; `ElctlDb.PostgresMigrateNeedsBackedUp` (skipped without `NOBION_TEST_PG_URL`): without the flag exit non-zero and stdout contains `pg_dump --format=custom` and not the password; with it, migrates.
- [ ] **Step 2: Run** `nobion_persistence_tests --gtest_filter='Migrate.*'`, `nobion_elctl_tests --gtest_filter='ElctlDb.*'` — FAIL.
- [ ] **Step 3: Implement** spec 11 (`VACUUM INTO` before the first transaction; quote doubling; refuse an existing target).
- [ ] **Step 4: Run** — PASS, and with `NOBION_TEST_PG_URL` set (Postgres.app on the developer Mac; its test database is still named `pychron_test` there, so the URL names that).
- [ ] **Step 5: Commit** `feat(persistence)!: a store is backed up before it is migrated`, footer `BREAKING CHANGE: elctl db migrate refuses a PostgreSQL store with pending migrations unless --backed-up is given; back it up with pg_dump first.` (release-please puts it in the release notes, spec §11)

---

## Branch 8: `ci/release-opencv` (Task 21)

### Task 21: Trimmed static OpenCV in releases; Arrow off

**Files:**
- Create: `packaging/opencv/build.py`, `packaging/THIRD_PARTY_NOTICES.md` (OpenCV Apache-2.0 text), `tools/tests/test_opencv_build.py`
- Modify: `.github/workflows/release.yml` (cache restore, build on miss, configure with `-DNOBION_VISION_OPENCV=ON -DOpenCV_DIR=... -DNOBION_PUBLISH=OFF`), `cmake/NobionPackaging.cmake` / `cmake/NobionInstall.cmake` (install the notices file), `docs/release_format.md` (Parquet needs a build with Arrow), release notes template if one exists

- [ ] **Step 1: Write the failing test** `tools/tests/test_opencv_build.py`: `build.py --print-cmake-args --os macos|linux|windows` prints exactly the option set of spec 12.1 for that OS (backends per OS; every `WITH_*` listed off is `OFF`; `BUILD_SHARED_LIBS=OFF`; `BUILD_LIST=core,imgproc,videoio`); the pinned tarball's sha256 is a 64-hex constant.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** the script (download, verify sha256, configure, build, install into `<prefix>`) and the workflow steps (cache key `opencv-<os>-<arch>-<version>-<sha256 of build.py>`).
- [ ] **Step 4: Verify locally** on macOS: `python3 packaging/opencv/build.py <src> <prefix>`, then configure a release-config build with `-DOpenCV_DIR=<prefix>/lib/cmake/opencv4 -DNOBION_VISION_OPENCV=ON -DNOBION_PUBLISH=OFF`, build, run `nobion_vision_tests` and `elctl --version` (features show `vision_opencv`, not `publish_parquet`); `otool -L` on `nobion` lists no OpenCV dylib. **Measure** installer size and release job time, cold and warm cache, before and after; record here.
- [ ] **Step 5: Commit** `ci: releases link a trimmed static OpenCV and build without Arrow`

---

## Measurements (filled in while building)

| Task | What | Before | After |
|---|---|---|---|
| 3 | `Power` write, 4 KiB, mean / p99 ms | — | |
| 8 | example queue `--sim-speed max`, Release vs RelWithDebInfo | | |
| 9 | `ctest --preset dev` wall; queue `--sim-speed max` | | |
| 12 | nobion `--sim` CPU %, records/s, recorder off vs on | | |
| 21 | installer size per OS; release job time cold / warm | | |

## Open questions

None.
