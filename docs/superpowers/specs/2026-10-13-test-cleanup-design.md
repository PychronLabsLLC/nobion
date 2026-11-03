# Test cleanup: what counts as superfluous

Draft for T036 ("cleanup tests. remove superfluous or excessive tests").
Agreed with owner 2026-10-13 (section 7). Nothing removed yet. UI suites
(`tests/ui`) out of scope.

## 1. Why

- Size: about 4600 GoogleTest cases (`TEST`, `TEST_F`, `TEST_P` at line
  start, `tests/` and `apps/elctl/tests`) plus about 830 QtTest slots in
  `tests/ui` (51 files). Tests 141 000 lines against 176 000 of source.
- Time (`ctest --preset dev-ui`, 2026-10-13): 932 s summed, about 8 min wall.
  Importer suites a third: `Engines/ProjectImportTest` 82 tests 243 s,
  `Engines/MetaImportTest` 26 tests 40 s, `ImportCmd` with parity 50 tests
  36 s. Then `LabSim` 9 tests 41 s, `ui.test_experiment_window` 31 s,
  `LabSessionTest` 27 tests 26 s, `ui.test_flux_window` 24 s.
- `dev-quick` already leaves the importer suites out
  (`CMakePresets.json:176`–`183`), so the cost lands on every full run
  before a merge and on CI.

Goal: same protection, less time and fewer lines to read. Removing a test is
a last resort; making it cheaper or folding it into a table comes first.

## 2. Rules that must hold

- Root `AGENTS.md` stands: a failing test is never skipped or disabled; a
  test failing only in parallel is fixed, not serialized; `*Steady` tests
  (`ExampleLineSteady`, `NgxLinkSteady`, `QtegraAcquireSteady`,
  `AdcBankSteady`), the pacing tests of `VirtualClock` and
  `AnInterruptStopsAPacedQueueInRealTime` are kept by design.
- A test that once caught a bug stays (its commit history or a comment says
  so), whatever the rules below say.
- Each removal or merge commit names the test(s) it drops and the test(s)
  that still cover the same behaviour. One suite per commit; full
  `ctest --preset dev` (or `dev-ui`) before each.
- A file-format test (bytes, sha, `TOML_FLOAT_CHARCONV`, legacy model
  strings, changeset messages) and a test against a fixture written on
  another platform are never removed.
- PostgreSQL runs of `TEST_P` suites (`store_fixture.hpp:32`) are kept: the
  instance count is the engines', not duplication.
- Coverage (lines and branches of `libs/`) measured before and after each
  suite's change does not drop. Measured how: section 5.

## 3. What counts as superfluous (proposal)

A test is a candidate when one of these holds and section 2 lets it go:

S1. **Asserts nothing behaviour-shaped**: only that a constructor's default
    or a getter returns what was set, with no rule behind it. (A default that
    is a rule, such as `DefaultsComeFromTheSchema`
    `tests/processing/test_options.cpp:29`, is not one.) About 60 names look
    like this; each read before it is called one.
S2. **A one-literal copy**: two or more tests whose bodies differ in a value
    and an expectation. Folded into one table test or `TEST_P`, not deleted.
    Candidates: `tests/devices/spectrometer/test_thermo_qtegra_acquire.cpp:88`–`146`,
    `tests/devices/spectrometer/legacy/test_acquirers.cpp:176`–`207`.
S3. **Covered twice at the same level**: same behaviour, same layer, same
    inputs, different name. Candidates by name, not yet by body:
    `SecondRunIsNoOp`, `OneHistoryOneResult`, `VerifyAfterImportIsOk`,
    `OpenAndPlanErrors`, `UnreadableVersionIsSupersededByAReadableOneOrADeletion`
    in `tests/dvc/test_project_import.cpp`, `test_meta_import.cpp`,
    `test_catalog_db.cpp`. Likely per-format twins (different code paths):
    kept unless their bodies show the same path.
S4. **CLI test that re-proves the engine**: an `elctl` test whose assertions
    are the engine test's, through the command line. Kept: one per command
    path (argument parsing, exit status, output text); assertions on the
    engine's result beyond that move to the engine test. Candidates:
    `ImportCmdParity` (`apps/elctl/tests/test_import_cmd.cpp:1002`, 13 tests)
    against `InterpretedAgeBecomesARevision`
    (`tests/dvc/test_project_import.cpp:1857`).
    Result (plan Task 8): candidate mostly wrong. `ImportCmdParity` is only
    test of age function `AgeAsOf` (`apps/elctl/src/import_age.cpp`); engine
    tests script `AgeFn`; `libs/ingest` does not reduce, so nothing can move
    there. As-of and not-comparable assertions kept; only those engine test
    already held removed, no test dropped.

Not superfluous: a UI test of what a library test also covers, when the UI
test checks the window's wiring (an action enabled, a status line, a
signal). Long tests are not superfluous for being long.

## 4. Making the slow ones cheap (proposal, before any removal)

- `ProjectImportTest` (`tests/dvc/test_project_import.cpp:319`),
  `MetaImportTest` (`tests/dvc/test_meta_import.cpp:299`) and `ImportCmd`
  (`apps/elctl/tests/test_import_cmd.cpp:82`) rebuild a git repo, a store and
  a catalog in every `SetUp`. No `SetUpTestSuite` exists in the tree.
  First measure `SetUp` against the test body (section 5). If `SetUp`
  dominates: build the repo and the empty store once per suite and copy the
  directory (and the SQLite file) per test; PostgreSQL gets a template
  database (`CREATE DATABASE ... TEMPLATE`). A test that only reads
  (`VerifyAfterImportIsOk` :2708, `OneHistoryOneResult` :2564) may share one
  imported world.
- Rule: a shared world is never written by a test; a test that writes gets a
  copy. A test that fails only when run with its suite is a bug (root
  `AGENTS.md`).
- Measured 2026-10-13 (`pychron_dvc_tests`, Debug, SQLite, M-series Mac,
  one process): `ProjectImportTest` 82 tests 125 s, median 0.49 s, fastest
  0.17 s; `MetaImportTest` 26 tests 34 s, median 0.28 s, fastest 0.13 s.
  `SetUp` is at most the fastest test, so at most about 10% of a suite:
  sharing it per suite is not the first thing. Three bodies hold 52 s of
  `ProjectImportTest` (`OneHistoryOneResult` 28 s,
  `ReadableRecordAfterAnUnreadableFirstRecordStartsTheCollection` 18 s,
  `VerifyAfterImportIsOk` 7 s) and 27 s of `MetaImportTest`
  (`OneHistoryOneResult` 18 s, `VerifyAfterImportIsOk` 5 s,
  `UnreadableVersionIsSupersededByAReadableOneOrADeletion` 4 s). So:
  profile those bodies first; a shared world only where `SetUp` is shown
  to matter. `OneHistoryOneResult` is kept whole whatever it costs
  (`libs/ingest/AGENTS.md`): made cheaper, never split or cut.
- Profiled 2026-10-16 (plan Task 3; same build and Mac; other nightly lanes
  running, load average 8 to 10 on 18 cores, so absolute times about 1.5x
  the 2026-10-13 ones: `ProjectImportTest` 190 s, `MetaImportTest` 43 s;
  shares below are what counts). How: `sample` on main thread from process
  start, each self sample charged to outermost test-level helper under
  `TestBody` (`run_import`, verify, fixture commits, `fresh_world`,
  `snapshot_of`) and split into waiting on a git child (`poll`,
  `posix_spawn`, `wait4` under `GitReader` or `GitFixture`) and in-process
  work; git processes counted with a logging `git` first on `PATH` (each
  `GitReader::open` runs `git --version` once, so those lines count adapter
  opens). Scripts and raw reports were scratch, not kept.
- Where time goes, whole suite (share of main-thread samples):

  | | `ProjectImportTest` | `MetaImportTest` |
  |---|---|---|
  | git children of `run_import` | 61.6% | 55.0% |
  | git children of verify | 8.1% | 11.3% |
  | fixture commits (`GitFixture`, `LegacyRepoBuilder`) | 14.6% | 12.7% |
  | importer, store, assertions in process | 10.5% | 15.7% |
  | other git waits in body | 0.6% | 1.2% |
  | `SetUp` | 2.0% | 2.3% |
  | `fresh_world` (migrations) | 1.5% | 0.9% |
  | teardown | 1.1% | 0.9% |
  | all waits on git children | 85.4% | 80.9% |

- Git processes per suite run: project 14 056 (772 adapter opens, 584
  fixture commits, 88 `init`), meta 3 720 (190 opens, 128 commits). One git
  process costs about 7 ms on this Mac (`git rev-parse HEAD`, 200 in
  1.47 s). By kind, project: per batch `diff-tree` 2 702, `log` 2 600,
  `cat-file` 2 331 (`GitReader::changes`, `commits`, `fetch_blobs`, one
  each per batch, already `--stdin`); per open 4 (`--version`,
  `rev-parse --git-dir`, `rev-parse --is-shallow-repository`,
  `rev-parse --verify <branch>`, `git_reader.cpp:388`–`453`), per run
  `rev-list` 993 and `for-each-ref` 690; per fixture commit 3 (`add -A`,
  `commit`, `rev-parse HEAD`, `git_fixture.hpp:140`–`147`).
- Six slowest bodies (one at a time, same load):

  | Test | time | git processes | adapter opens | fixture commits | worlds | git wait |
  |---|---|---|---|---|---|---|
  | project `OneHistoryOneResult` | 30 s | 3 211 | 119 | 35 + 2 merges | 10 | 87% |
  | project `ReadableRecordAfterAnUnreadableFirstRecordStartsTheCollection` | 20 s | 2 109 | 106 | 16 | 15 | 87% |
  | project `VerifyAfterImportIsOk` | 7 s | 699 | 8 | 37 + 2 merges | 2 | 86% |
  | project `UnreadableVersionIsSupersededByAReadableOneOrADeletion` | 7 s | 782 | 44 | 11 | 8 | 89% |
  | meta `OneHistoryOneResult` | 20 s | 1 944 | 79 | 29 + 4 merges | 8 | 84% |
  | meta `VerifyAfterImportIsOk` | 6 s | 573 | 8 | 29 + 4 merges | 2 | 83% |
  | meta `UnreadableVersionIsSupersededByAReadableOneOrADeletion` | 5 s | 427 | 34 | 5 | 8 | 78% |

  Worlds: `world_` of `SetUp` plus each `fresh_world()`. Opens: every import
  run and every verify walk opens a new adapter, hence a new `GitReader`.
  `OneHistoryOneResult`: 85 of project's 119 opens are the stopped-after-
  every-batch runs, replays 53% of its time (meta 51%). `ReadableRecord...`
  calls `same_at_every_cut` twice (7 worlds, about 52 opens each). Verify
  tests: verify walks 67% of time, fixture commits 19%.
- `SetUp` alone (scratch `TEST_P` with empty body, `--gtest_repeat=20`, not
  committed): project median 34 ms (33 to 41), meta 32 ms (31 to 36): 2.2%
  and 2.4% of 2026-10-13 suite sums. 10% bound of 2026-10-13 holds, with room: shared
  per-suite world, template store, template database not worth doing; out
  of this work.
- Not the cause: migrations, `snapshot_of`, history reads, store writes
  (each 2% or less).
- So two causes, in order:
  1. Git processes `GitReader` starts for each import run and verify walk
     (70% project, 66% meta). Tests drive many short runs on purpose
     (cut, resumed, replayed: `libs/ingest/AGENTS.md`), so fixed cost of a
     run counts many times: 4 processes to open, 2 per run, 3 per batch.
     Remedy (plan Task 4): fewer processes per open and per batch, same
     output: `git --version` once per process; the three `rev-parse` of
     `open` as one; `commits` and `changes` of a batch from one `git log`.
     Production code (`libs/dvc`), behaviour unchanged, speed change
     measured as root `AGENTS.md` asks; helps `elctl import` and the
     `ImportCmd` suites too. Not chosen: a memoizing process runner
     injected in tests (larger cut, but GitReader would then parse output
     git did not produce for that call).
  2. Fixture commits (13 to 15%; 19% of verify tests). Remedy (plan
     Task 5): 3 processes per commit to 2: sha read from the ref file in
     fixture's own git dir instead of `rev-parse HEAD`, `rev-parse` kept
     as fallback when ref is packed.
- `LabSim`, `LabSessionTest`, `ui.test_experiment_window`: look for real
  waits that a `VirtualClock` would remove before anything else.

## 5. Measuring

- Time per test: `ctest --preset dev -j1 --output-junit` timings, and
  GoogleTest's own per-test times; `SetUp` timed with a scoped timer in the
  fixture under an environment variable (`PYCHRON_TEST_TIMING`), not left on.
- Coverage: a `coverage` configure preset (`-fprofile-instr-generate
  -fcoverage-mapping`, clang), `llvm-cov` summary (`export -summary-only`,
  `tools/coverage_report.py`) on `libs/` before and after each suite's
  change; the two reports in the commit message. Not in CI.
- Per-test unique coverage (which test is the only one to reach a line) is
  out of reach without per-test profiles; S3 is judged by reading bodies.

## 6. Order of work (for the plan)

1. Measurement tooling (timing hook, coverage preset), no test changed.
2. Importer suites made cheaper where section 4's profile puts the time:
   fewer git processes per import run, then per fixture commit; times
   before and after. (First proposed: fixtures shared per suite; profile
   showed `SetUp` 2%, so dropped.)
3. S2 folds, one file per commit.
4. S4: `ImportCmdParity` trimmed to command paths.
5. S1 and S3 removals, one suite per commit, each with its named cover.

## 7. Decisions for owner

1. Goal in numbers: a wall-time target for `ctest --preset dev`, or only "less"?
2. Rules S1 to S4: agreed, changed, or more?
3. Section 4 before any removal: agreed?
4. Coverage gate (section 2, last rule): wanted, given it needs a local
   coverage build?
5. UI suites (`tests/ui`) in scope, or library suites only for now?

Owner's answers (2026-10-13): (1) less, no number; (2) S1 to S4 agreed;
(3) section 4 before any removal: agreed; (4) coverage gate wanted; (5)
library suites only: `tests/ui` out of scope.

## 8. Out of scope

- Adding tests for gaps found on the way: noted in the task inbox, not done
  here.
- CI matrix changes, test sharding, ccache.
- Changing what `dev-quick` leaves out.
