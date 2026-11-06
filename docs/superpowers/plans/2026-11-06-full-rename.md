# Full Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename every internal `pychron` name to `nobion` by one scripted rule with an explicit exception list, and keep it renamed with a check.

**Architecture:** A Python script applies the case-preserving substring rule to every tracked path and text file, protected by `tools/name_keep.txt`; its output is one generated commit. Hand fixes, a pre-push check, and a one-off tool for the developer's own machine follow.

**Tech Stack:** Python 3.11 (standard library only, `unittest`), git, CMake, ctest.

**Spec:** `docs/superpowers/specs/2026-11-06-full-rename-design.md` (read it whole).

## Global Constraints

- Rule: `pychron` → `nobion`, `Pychron` → `Nobion`, `PYCHRON` → `NOBION`, substring, case preserved, in tracked paths and tracked text files; binary files never touched.
- Exceptions come only from `tools/name_keep.txt` (spec §3), three kinds: `keep: <glob>` (kept whole, content and name); `identifiers: <glob> [+<extra token>]...` (only these regex classes replaced: `\bpychron::`, `\bnamespace pychron\b`, `#include ["<]pychron/`, `\bPYCHRON_(?=[A-Z])`, and in `CMakeLists.txt`/`*.cmake` `\bpychron(?=[_:][a-z])`; directory components renamed by the rule, a file's own name kept); `string: <exact text> [@ <glob>]` (case-sensitive, kept everywhere or only in the scoped files). Precedence for a path: `keep:` > `identifiers:` > rule; first matching line of a kind wins. Each entry ends with `  # <reason>`.
- Kept strings are protected in one pass: a single regex alternation of all kept strings that apply to the file, longest first, replaced through a callback (no placeholders). A kept string that is the prefix of a longer identifier (`pychron_valves_extra`) does not protect it: kept strings match only at identifier boundaries (`(?<![A-Za-z0-9_])` … `(?![A-Za-z0-9_])`) unless the entry ends with `*`.
- A file is binary, and never rewritten, when `git check-attr binary text -- <path>` says `binary: set` or `text: unset`, or it has a NUL in its first 8000 bytes. Symlinks (mode 120000) and submodules (160000) are skipped.
- Paths are moved per file: the full new path computed from the old, parent directories created, `git mv`; empty old directories removed after; untracked files never touched (the script refuses to run with any untracked or modified file).
- Tests that create git repositories set `GIT_CONFIG_GLOBAL=/dev/null`, `GIT_CONFIG_NOSYSTEM=1`, and pass `-c user.name=t -c user.email=t@t -c core.autocrlf=false`.
- `libs/persistence/migrations/**` byte-identical to `origin/develop`.
- No compatibility code in the product (spec §4).
- Branch `refactor/rename-to-nobion` from `origin/develop`. Lands by root `AGENTS.md` workflow, with the build directories configured from scratch.
- Script, check and tool run with `python3 -I`; no third-party modules.

## Branch order

One branch. Field diagnostics (`2026-11-06-field-diagnostics.md`) restarts after it lands; `feat/core-build-identity` and its worktree are deleted in Task 3.

## Review Focus

1. **A name that only looks legacy**: `pychron` inside a file under an `identifiers:` glob that is really this program's (a log line `"pychron"` logger name in `apps/elctl/src/import.cpp`). Expect: the full test run or `check_names.py` does not catch it; the reviewer of Task 2 greps `identifiers:` paths for `"pychron` string literals and lists each with a keep/rename verdict. Test in Task 2 Step 4.
2. **Kept string adjacent to a renamed one** (`PychronValves` next to `pychron::devices`, `pychron_valves` next to `pychron_core_tests`). Expect both handled correctly in one line. Test in Task 1 (`test_kept_string_beside_renamed_token`).
3. **Path rename collisions**: two old paths mapping to one new path, or a new path already existing. Expect: the script refuses before changing anything. Test in Task 1 (`test_refuses_a_collision`).
4. **CRLF files, files without trailing newline, non-UTF-8 text** (Windows `.rc`, `.bat`, legacy fixtures). Expect: bytes other than the replaced ones unchanged. Test in Task 1 (`test_preserves_line_endings_and_encoding`).
5. **A reused build tree** after the rename. Expect: docs tell the developer to delete it; `rename_local_state.py` prints the command. Covered in Tasks 3 and 5.

---

### Task 1: The rename script, its exception list, and the check

**Files:**
- Create: `tools/name_keep.txt`, `tools/rename_to_nobion.py`, `tools/check_names.py`, `tools/naming.py` (shared parser and matcher), `tools/tests/test_rename_to_nobion.py`, `tools/tests/test_check_names.py`
- Modify: `tests/core/CMakeLists.txt` (ctest `tools.rename_to_nobion` running its unittest, as `tools.quality_check` does)

**Interfaces:**
- Produces: `python3 -I tools/rename_to_nobion.py [--root DIR] [--dry-run]` (exit 0 done, 1 refused with reason, 2 bad arguments); `python3 -I tools/check_names.py [--root DIR]` (exit 0 clean, 1 findings as `path:line: pychron`, 2 bad keep file). `tools/naming.py`: `load_keep(path) -> Keep`, `Keep.kind_of(path) -> 'keep'|'identifiers'|'rename'`, `rename_text(text: str, path: str, keep: Keep) -> str`, `new_path(path: str, keep: Keep) -> str`.

- [ ] **Step 1: Write the failing tests** (each builds a temp git repo with `git init` and a few files)
  - `test_renames_namespace_include_macro_target`: `namespace pychron::core {`, `#include "pychron/core/x.hpp"`, `PYCHRON_SANITIZE`, `pychron_core_tests` → nobion forms.
  - `test_preserves_case`: `Pychron`, `PYCHRON`, `pychron`, `RoundTripsPychronExamples` → `Nobion`, `NOBION`, `nobion`, `RoundTripsNobionExamples`.
  - `test_moves_paths_deepest_last`: `libs/a/include/pychron/a/x.hpp` → `libs/a/include/nobion/a/x.hpp`; `cmake/PychronWarnings.cmake` → `cmake/NobionWarnings.cmake`; rename detected: a 30-line file where one line changes, after `git add -A`, `git diff --cached -M --name-status` shows `R`.
  - `test_keep_glob_untouched`: a file under a `keep:` glob is byte-identical, name included.
  - `test_identifiers_glob_renames_tokens_only`: in an `identifiers:` file, `namespace pychron`, `#include "pychron/x.hpp"`, `PYCHRON_X`, `pychron_core` renamed; `legacy pychron writes`, `"pychron.ausgeochem"`, `~/.pychron/foo` unchanged; `include/pychron/` directory renamed but a basename `pychron_tx.hpp` kept; `#include <pychron/x.hpp>` renamed; `PychronLegacyThing` kept; a `+#! pychron:` extra token renamed; a path matching both a `keep:` and an `identifiers:` glob is kept whole (precedence).
  - `test_kept_string_beside_renamed_token`: `PychronValves v; pychron::devices::x; "pychron_valves"; pychron_core_tests` → only the second and fourth change.
  - `test_overlapping_kept_strings`: kept `PychronLabs` and `PychronLabsLLC`, text `PychronLabsLLC/x PychronLabs y` unchanged; kept `pychron_tx`, text `pychron_tx_tests` renamed to `nobion_tx_tests` (boundary rule).
  - `test_scoped_string_only_in_its_files`: `string: "Pychron" @ a.hpp` keeps it in `a.hpp`, renames it in `b.hpp`.
  - `test_gitattributes_binary_untouched`: a `-text` file without NUL keeps its bytes.
  - `test_refuses_a_dirty_tree`: an untracked file → exit 1, nothing changed.
  - `test_binary_untouched`: a file with a NUL byte containing `pychron` unchanged.
  - `test_preserves_line_endings_and_encoding`: CRLF file, no-trailing-newline file, Latin-1 file: only the replaced bytes differ.
  - `test_refuses_a_collision`: `a/pychron.txt` and `a/nobion.txt` both exist → exit 1, tree unchanged.
  - `test_is_idempotent`: second run on the output changes nothing (`git status --porcelain` empty after commit).
  - `test_dry_run_changes_nothing`.
  - `test_check_finds_a_stray_name`: `check_names.py` exit 1 with `path:line`; clean tree exit 0; a `pychron` in an `identifiers:` file outside its tokens → 0; a token there → 1; a path containing `pychron` outside keep/identifiers globs → 1; a file holding only kept strings (`PychronLabs`, `legacy pychron`, `PychronValves`) → 0.
  - `test_keep_file_entries_need_a_reason`: an entry without `  # reason` → exit 2.
- [ ] **Step 2: Run** `python3 -I -m unittest discover -s tools/tests -p 'test_rename_to_nobion.py'` and `... 'test_check_names.py'` — FAIL.
- [ ] **Step 3: Implement.** Files to consider: `git ls-files -z`. Text detection: no NUL in the first 8000 bytes (as git does). Work on bytes; replacement applied to the byte strings `pychron`/`Pychron`/`PYCHRON` (ASCII, so any ASCII-compatible encoding is safe). Paths: compute every new path first, refuse on a collision or an existing target, then move per file (Global Constraints). `name_keep.txt` holds every entry of spec §3, each with its reason. Before writing it, two audits on the current tree, both recorded in the report: (a) each `tests/*/fixtures/` directory: legacy data (keep) or this program's format (rule), with the evidence; (b) `git grep -in pychron` over files the rule would rewrite, for prose about the old program (words such as replacement, port, ported, original, legacy, from), e.g. `vcpkg.json` "(pychron replacement)": each becomes a `string:` entry or a Task 3 hand fix. The exact test names built on legacy classes are found with `git grep -n 'Pychron' -- tests | grep -i 'legacy\|valve\|tx'` and listed.
- [ ] **Step 4: Run** both unittests — PASS. Run `python3 -I tools/rename_to_nobion.py --dry-run` on the real tree and save its summary (paths moved, files changed, per-kind counts) to `docs/superpowers/plans/2026-11-06-full-rename.md` under "Dry run" below.
- [ ] **Step 5: Commit** `chore(tools): a script that renames pychron to nobion, and a check that keeps it so`

### Task 2: The generated rename

**Files:** everything the script touches.

- [ ] **Step 1:** `python3 -I tools/rename_to_nobion.py` on the branch tip (after Task 1's commit). Then `git add -A` and commit `refactor!: rename pychron to nobion inside the code` with the footer of spec §5.2 and the trailer. No hand edit in this commit.
- [ ] **Step 2:** `python3 -I tools/check_names.py` — exit 0. `git diff origin/develop -- libs/persistence/migrations` — empty. `python3 -I tools/rename_to_nobion.py --dry-run` — nothing to do.
- [ ] **Step 3:** Fresh build: `rm -rf build/dev-ui && cmake --preset dev-ui && cmake --build --preset dev-ui`, then `ctest --preset dev-ui`. Record every failure in the task report; do not fix here (Task 3 does).
- [ ] **Step 4 (Review Focus 1):** `git grep -n '"[^"]*pychron[^"]*"' -- <every identifiers: glob>` and list each hit in the report with keep (legacy) or rename (this program's) and why; every "rename" becomes a Task 3 item.
- [ ] **Step 5:** The commit stands as generated; report its stat (`git show --stat | tail -1`).

### Task 3: Hand fixes

**Files:**
- Delete: `apps/nobion/src/settings_migration.{hpp,cpp}` (listed at `apps/nobion/CMakeLists.txt:120`; included and called in `apps/nobion/src/main.cpp:105` and below), `tests/ui/test_settings_migration.cpp` (UI tests are globbed by `tests/ui/CMakeLists.txt`)
- Modify: `packaging/macos/cpack_sign.cmake:16` (`/Nobion\.app$`), `docs/dev_setup.md` (fresh build tree after the rename; steps for another branch, spec §7), `docs/superpowers/specs/2026-11-06-field-diagnostics-design.md` and `docs/superpowers/plans/2026-11-06-field-diagnostics.md` (by hand, new names; they are under a `keep:` glob, so the script did not), whatever Task 2 Steps 3–4 listed
- Not the implementer's: the controller removes branch `feat/core-build-identity` and its worktree (made by the controlling session for field diagnostics Task 1; spec §7 decides it) with `git worktree remove` (no `--force`; if refused, it reports what blocks it and asks) and `git branch -D` (unmerged by design: the WIP is redone after the rename)

- [ ] **Step 1:** For each Task 2 failure, the fix with the test that failed as its proof; one commit per cause (`fix: …` or `refactor: …`).
- [ ] **Step 2:** The deletions and `cpack_sign` fix; `nobion --self-test` passes.
- [ ] **Step 3:** The two field diagnostics documents: every `pychron`/`Pychron`/`PYCHRON` name that is this program's becomes the `nobion` form (`pychron_core_tests` → `nobion_core_tests`, `PYCHRON_HARDENED_STDLIB` → `NOBION_HARDENED_STDLIB`, `pychron_build_info` → `nobion_build_info`, …); legacy references unchanged. Commit `docs: field diagnostics in the new names`.
- [ ] **Step 4:** `ctest --preset dev-ui` — PASS (all). Commit each step as it passes.

### Task 4: The check runs on every push

**Files:**
- Modify: `tools/githooks/pre-push` (run `python3 -I tools/check_names.py` first, before the hook's early exits for a missing `quality_check.py` and for a push without C++, so a docs-only push is checked too; a finding refuses the push), `tests/core/CMakeLists.txt` (ctest `tools.check_names` and `tools.rename_local_state` running their unittests, as `tools.quality_check` does; and `tools.check_names.tree` running the check on the source tree, skipped when `${PROJECT_SOURCE_DIR}/.git` does not exist; a stray name on a branch failing ctest is intended), root `AGENTS.md` (one compressed line under Static analysis: the check, `tools/name_keep.txt`, never extended to get a change through)

- [ ] **Step 1:** Add a stray `pychron` to a scratch file in a throwaway commit; `tools/githooks/pre-push` refuses; drop the commit.
- [ ] **Step 2:** `ctest --preset dev-ui -R tools.check_names` — PASS.
- [ ] **Step 3: Commit** `chore(tools): a push with a pychron name in it is refused`

### Task 5: Moving the developer's own state

**Files:**
- Create: `tools/rename_local_state.py`, `tools/tests/test_rename_local_state.py`

**Interfaces:**
- Produces: `python3 -I tools/rename_local_state.py [--dry-run] [--home DIR]` (`--home` for tests: every per-user path is resolved under it, using the same platform rules as `libs/core/src/log_dir.cpp`, `libs/setup/src/site.cpp`, `libs/earthbank/src/profiles.cpp`, `apps/elctl/src/import_sources.cpp`).

- [ ] **Step 1: Write the failing tests** (temp home with the old layout: `Library/Logs/pychron/pychron.log`, `Library/Application Support/Pychron/{site.toml,examples/,earthbank/profiles.toml}`, an install root with `.pychron/install.toml`, `.pychron/credentials.toml`, `data/pychron.db`, `site.toml` naming it with `database = "sqlite:<root>/data/pychron.db"`, `~/pychron-logs/`)
  - `test_moves_everything_and_rewrites_urls`: new layout present, old gone, `site.toml`/`install.toml`/`credentials.toml` name `.nobion` and `nobion.db`.
  - `test_refuses_when_a_target_exists`: nothing moved.
  - `test_dry_run_changes_nothing_and_lists_steps`.
  - `test_prints_but_does_not_run_external_steps`: output names `NOBION_TEST_PG_URL`, `ALTER DATABASE pychron_test RENAME TO nobion_test`, `ALTER ROLE pychron RENAME TO nobion`, keychain items, `rm -rf build/` lines.
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement.** **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `chore(tools): a one-off move of a developer's own state to the new names`

### Task 6: Checks before landing

- [ ] **Step 1:** Every configure preset from a fresh directory: `for p in $(cmake --list-presets | awk '/"/{gsub(/"/,"",$1); print $1}'); do cmake --preset $p -B "$SCRATCH/preset-$p" >/dev/null || echo "FAIL $p"; done` with `SCRATCH` the session's scratch directory (`mac-debug` and `mac-release` need vcpkg and are expected to be skipped, with the reason). `actionlint .github/workflows/*.yml` when installed.
- [ ] **Step 2:** ASan/UBSan: `cmake --preset dev -B build/dev-asan -DNOBION_SANITIZE=address,undefined`, build and run `nobion_core_tests`, `nobion_devices_tests`, `nobion_persistence_tests`, `nobion_scripting_tests` (LSan patterns in `cmake/lsan.supp` now `nobion::…`).
- [ ] **Step 3:** `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (whole tree) — 0 warnings. `python3 tools/quality_check.py --build-dir build/dev-ui` — findings only on lines whose sole change is the rename and that exist at the same place on `origin/develop` (list them in the report with that evidence); any other finding fixed.
- [ ] **Step 4:** `elctl init argus --yes --root <tmp>`: `<tmp>/.nobion/install.toml` and `<tmp>/data/nobion.db` exist; `elctl --install <name> doctor` passes; `nobion --self-test` passes. Metrics: add `[metrics]` `enabled = true`, `bind = "127.0.0.1"`, `port = 19464` to the install's `extraction_line.toml`, start `QT_QPA_PLATFORM=offscreen nobion --install <name> --sim` in the background, `curl -s 127.0.0.1:19464/metrics | grep -c '^pychron_'` is 0 and `grep -c '^nobion_'` is above 0, stop it.
- [ ] **Step 5:** `python3 -I tools/check_names.py` — 0; `rename_to_nobion.py --dry-run` — nothing to do; migrations diff empty. Record all results in the report. No commit.

## After the owner runs Task 5's tool

A later commit (not this plan) deletes `tools/rename_local_state.py`, its test and its keep entries.

## Dry run

(Filled by Task 1 Step 4.)

## Open questions

None.
