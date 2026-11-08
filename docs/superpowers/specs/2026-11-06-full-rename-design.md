# Full rename: pychron → nobion inside the code

Date: 2026-11-06. Deciders: Jake Ross.
Status: rename built (`01d1f197` to `698c8a05`); script, exception list, check and local-state tool not built. What landed and what is owed: section 9.
Style: compressed (root `AGENTS.md`). Nothing technical dropped.

## 1. Why, and why now

`2026-11-03-nobion-app-design.md` renamed what a user sees (program, bundle, installers, UI text) and left inside names out of scope: `pychron::` namespace, `PYCHRON_*` macros and env vars, `pychron_*` targets, `libs/*/include/pychron`, `share/pychron`, `.pychron`. Owner, 2026-11-06: rename all of it.

Size on develop at `8d3ad1da`, outside `docs/superpowers`: about 1,900 files and 13,900 lines mention pychron; about 1,550 files open `namespace pychron`; about 1,430 include `pychron/...`; about 200 distinct `PYCHRON_*` names; about 80 `pychron_*` names in CMake; about 45 metric names; 28 `include/pychron` directories; 9 `cmake/Pychron*.cmake` files.

Now: nothing deployed, so no lab migrates. Every week later adds code in the old names. Field diagnostics (`2026-11-06-field-diagnostics-design.md`) paused until this lands.

## 2. The rule

In every tracked text file, and in every tracked path, replace by substring, preserving case: `pychron` → `nobion`, `Pychron` → `Nobion`, `PYCHRON` → `NOBION`. Substring, not word: `pychron::`, `PYCHRON_SANITIZE`, `pychron_core_tests`, `_pychron_runtime`, `include/pychron/`, `share/pychron`, `.pychron`, `data/pychron.db`, `pychron.log`, `#! pychron:`, `pychron_change`, `pychron-release/1`, `RoundTripsPychronExamples` all follow. Exceptions: section 3 only.

What this reaches, so nobody is surprised:

- Code: namespace, include root, macros, CMake options, variables, targets, aliases, modules (`cmake/Pychron*.cmake` → `cmake/Nobion*.cmake`), functions, install components (`pychron`, `pychron_qt`), `CPACK_PYCHRON_*`, generated sources built from CMake strings (`libs/persistence/CMakeLists.txt` writes `namespace pychron::persistence::detail`), `cmake/lsan.supp` symbol patterns.
- Names other programs or people use: env vars (`NOBION_SITE_CONFIG`, `NOBION_PROFILES_DIR`, `NOBION_EXAMPLES_DIR`, `NOBION_CCACHE`, `NOBION_TEST_*`, `NOBION_PROM_UID`, `NOBION_LAB`), the env vars a notification `[[command]]` hook receives (`NOBION_EVENT`, `NOBION_SUBJECT`, `NOBION_QUEUE`, `NOBION_IDENTIFIER`, `NOBION_ERROR`; `docs/notifications.md` follows), metric names (`nobion_*`; `packaging/observability` scrape config, dashboards, alerts follow), the Postgres `pg_notify` channel `nobion_change` and `application_name`, the advisory lock name, default user and database names (`nobion`, `nobion_test` in `ci.yml`, `profiles/store-common`, `profiles/instrument-common/experiment.toml`), the D-Bus secret name `org.nobion.earthbank.Password`, keychain items, the sim image `nobion-sim`.
- Files and folders: `share/nobion`, `<root>/.nobion/`, `data/nobion.db`, per-user dirs (`~/Library/Logs/nobion`, `~/Library/Application Support/Nobion` with its `examples` and `earthbank/profiles.toml`, `$XDG_STATE_HOME/nobion`, `$XDG_CONFIG_HOME/nobion`, `%LOCALAPPDATA%\nobion`, `%APPDATA%\Nobion`, the import cache `<cache>/nobion/import`), `nobion.log`, temp and probe names.
- Formats: script header `#! nobion:`; release manifest format `nobion-release/1`, identifier `nobion-release:<uuid>`, properties `nobion:*` (`libs/publish`; `docs/release_format.md`, `tests/publish/fixtures` follow; no release has been published); Python-visible names (`_nobion_runtime`, `<nobion-prelude>`, `<nobion-runtime>`, `__nobion_script__`); Qt object names and properties; log payload prefixes; messages ("not a nobion store").
- Packaging: `Info.plist.in` camera-permission text, entitlements comments, `.desktop`, NSIS strings, `release.yml` path filters and component names, `tests/setup/install_test.cmake`.
- Docs and rules: root and component `AGENTS.md`, `docs/` outside `docs/superpowers`, `README.md`, `configs/examples`, `profiles`.

## 3. Exceptions

`tools/name_keep.txt` lists every exception, each with a one-line reason; the rename script and the check (section 6) read the same file. Three kinds of entry: `keep:` a path glob kept whole (content and name); `identifiers:` a path glob where only code identifiers are replaced; `string:` an exact, case-sensitive text kept wherever it appears, or only in the files of an optional `@ <glob>`. Precedence for a path: `keep:` before `identifiers:` before the rule; `string:` entries apply inside `identifiers:` and rule files.

1. **Kept whole (paths):**
   - `libs/persistence/migrations/**`: applied migrations. A store records each applied file's checksum and refuses a file that differs (`libs/persistence/src/migrate.cpp`, `libs/persistence/AGENTS.md`); editing one breaks every existing store. Their `'pychron: …'` texts and comments stay.
   - `docs/superpowers/**`: dated specs and plans are history. (The field diagnostics spec and plan, not yet built, are updated by hand in this branch: section 5.)
   - `CHANGELOG.md`.
   - `tools/rename_to_nobion.py`, `tools/naming.py`, `tools/check_names.py`, `tools/name_keep.txt`, `tools/rename_local_state.py`, `tools/tests/test_check_names.py`, `tools/tests/test_rename_to_nobion.py`, `tools/tests/test_rename_local_state.py`: they must name the old word.
   - Data captured from legacy Pychron: `tests/traces/**`, `tests/data/**`, `tests/arar/golden/**`, and the `tests/*/fixtures/` directories whose content came from legacy Pychron (dvc, ingest and the others the plan's audit names, each listed by path). Fixtures of this program's own formats (for example `tests/publish/fixtures`) follow the rule.
2. **Identifiers only (`identifiers:` paths)**: code that reads or speaks legacy Pychron. Here only these tokens are replaced (each a regex class in the script): `pychron::`, `namespace pychron`, `#include "pychron/` and `#include <pychron/`, `PYCHRON_` followed by an uppercase letter, and in CMake files the `pychron_*` / `pychron::*` target and function names; per-glob extra tokens may be added. Every other "pychron" stays, camel-case identifiers such as `PychronLegacy…` included (prose such as "legacy pychron writes…", legacy keys, paths, module names). Paths here: every directory component is renamed by the rule (so `include/pychron/` moves with everyone else's); a file's own name is kept when it contains pychron (legacy protocol file names).
   - `libs/ingest/**`, `libs/dvc/**`, `apps/elctl/src/import*.{cpp,hpp}`, `libs/setup/**/legacy_line*`, `tools/pychron_conditionals_import.py`, `tools/pychron_measurement_import.py`, `tools/export_patterns.py`, `tools/tests/test_pychron_*`, `tests/ingest/**`, `tests/dvc/**`, `tests/scripting/scripts/**` (their `#! pychron:` header is added to the token list for this glob), `docs/legacy_import.md`.
   - Legacy-protocol driver and codec: `libs/codecs/**/pychron_tx*`, `libs/devices/**/pychron_valve*`, `tests/codecs/test_pychron_tx.cpp`, `tests/devices/test_pychron_valve*`. Their file names stay; their directories move.
3. **Kept strings (anywhere):**
   - driver kind `pychron_valves`, codec name `pychron_tx`, classes `PychronValves`, `PychronValveServerSim`, `PychronTxCodec`, `PychronValvesRig`, `PychronGPActuator`, and every test name built on them (`AnotherPychronsValvesAreAddressedByName`, `MatchesLegacyPychronOnEveryFixtureScan`, …: listed exactly in the file);
   - `legacy pychron`, `legacy Pychron`, `Legacy Pychron` (prose that names the old program, in any file);
   - the legacy repository `PychronLabsLLC/pychron`, legacy paths `~/Programming/pychron` in docs and test generators;
   - EarthBank's vocabulary value `"Pychron"` (a `string:` scoped `@` to `vocab.hpp` and the two tests below, not kept everywhere) in `kInterpretationTool` (`libs/earthbank/include/pychron/earthbank/vocab.hpp`) and the tests asserting it (`tests/earthbank/test_vocab.cpp`, `test_mapping.cpp`): the EarthBank template's list holds "Pychron" (`ArArDataPoint_Template2026.xlsx`); stays until EarthBank's list has Nobion;
   - EarthBank legacy profile section `pychron.ausgeochem` (`libs/earthbank/src/profiles.cpp`, `kLegacySection`) and its tests;
   - pickle module prefix `pychron.lasers.pattern` (`tools/export_patterns.py`);
   - derived-value fingerprint salt `pychron-derived-fingerprint/1` (`libs/persistence/src/refs.cpp`, `tests/arar/fixtures/fingerprint_before_move.txt`): an opaque hash input; renaming changes every cached fingerprint for no gain;
   - the RAISE texts `tools/ddl_sqlite.py` writes into `migrations/sqlite` (`pychron: … is append-only`, `… of a published release is frozen`): its CI check (`ddl_sqlite.py --check`) compares output with the frozen files;
   - company names (section 3.4 below).
4. **The company** (owner, 2026-11-06: kept): Pychron Labs LLC, GitHub organisation `PychronLabsLLC`, QSettings organisation `PychronLabs`, bundle identifier domain `com.pychronlabs`, `https://pychronlabs.com/...` (`libs/publish/src/records.cpp`). Kept strings.
5. `elctl` (no pychron in it).

## 4. No compatibility layer

Nothing is deployed: programs read only new names. No alias namespace, no fallback env var, no old path probed.

- `apps/nobion/src/settings_migration.{hpp,cpp}` and `tests/ui/test_settings_migration.cpp` (carry QSettings from `pychron-ui` to `nobion`, `2026-11-03-nobion-app-design.md`) deleted: the only machine they served has run them.
- Developer's own Mac: one-off `tools/rename_local_state.py [--dry-run]`, not built into anything (dropped, section 9: owner's Mac moved without it):
  - moves per-user dirs of section 2 (logs, Application Support with `examples` and `earthbank`, XDG dirs, import cache) to new names; refuses if a target exists;
  - for each install in the old `site.toml`: `<root>/.pychron` → `<root>/.nobion`, `data/pychron.db` → `data/nobion.db`; rewrites paths and `sqlite:` URLs inside the moved `site.toml`, `install.toml` and `credentials.toml`;
  - moves `~/pychron-logs` (from the old `configs/examples/logging.toml`) if present;
  - prints, does not run: the shell-profile lines to change (`PYCHRON_*` → `NOBION_*`, among them `PYCHRON_TEST_PG_URL`); `psql` commands to rename database `pychron_test` and role `pychron` (`ALTER DATABASE … RENAME TO nobion_test`, `ALTER ROLE … RENAME TO nobion`); keychain items to re-enter; `build/*` and worktree build trees to delete;
  - `--dry-run` prints every step and changes nothing.
  - Deleted from the tree once the owner has run it (a later commit says so).

## 5. How it is done

One branch, `refactor/rename-to-nobion`, from `origin/develop`. Commits:

1. `tools/rename_to_nobion.py`, `tools/name_keep.txt`, `tools/check_names.py`, their unittests (`tools/tests/test_rename_to_nobion.py`, `test_check_names.py`). Script: `git mv` of every path containing pychron outside kept and identifiers-only paths (directories first, deepest last); content rewrite of every tracked text file by section 2 except section 3; kept strings protected by placeholder before replacement and restored after; binary files never touched (detected as git does); idempotent (a second run changes nothing; unittest).
2. The script's output, one commit: `refactor!: rename pychron to nobion inside the code`, footer `BREAKING CHANGE: environment variables, metric names, files and folders are named nobion (NOBION_*, nobion_*, share/nobion, .nobion, nobion.log, data/nobion.db, #! nobion:).` Contains only what the script did; a reviewer reproduces it by running the script on the parent and diffing.
3. Hand changes, each its own commit: section 4 deletion; `packaging/macos/cpack_sign.cmake:16` still filters `/Pychron\.app$` though the bundle is `Nobion.app` (bug since the app rename: fixed to `/Nobion\.app$`); the field diagnostics spec and plan (`2026-11-06-field-diagnostics-design.md`, `2026-11-06-field-diagnostics.md`) rewritten in the new names; `docs/dev_setup.md` steps for other branches (section 7) and for a fresh build tree; anything the full test run shows the script could not do.
4. `tools/check_names.py` wired into `tools/githooks/pre-push` and a ctest entry `tools.check_names` (like `tools.quality_check`).
5. `tools/rename_local_state.py`.

Build trees: every build directory (each preset, each worktree) is configured from scratch after this branch: a reused `CMakeCache.txt` keeps `PYCHRON_*` entries the build no longer reads, so a reused `build/dev-asan` would silently drop sanitizers and `build/dev` warnings-as-errors. ccache's first build after the rename misses entirely (macro and include changes reach every translation unit): one full build's time (about 330 s on `dev`, longer on CI).

Checks before landing:
- `python3 tools/check_names.py` exits 0; `rename_to_nobion.py` on the result changes nothing.
- `git diff origin/develop -- libs/persistence/migrations` empty.
- Each preset configures from a fresh directory (`cmake --preset <name>` for every configure preset in `CMakePresets.json`); `actionlint` on `.github/workflows` when installed (CI itself first runs at the next `develop`→`main` pull request).
- Full `cmake --build --preset dev-ui`, `ctest --preset dev-ui`, `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (whole tree), and core, devices, persistence, scripting suites under `-DNOBION_SANITIZE=address,undefined` (with `cmake/lsan.supp` renamed patterns matching).
- `tools/quality_check.py`: a rename touches lines with old findings. A finding on a line whose only change is the rename, present at the same place on `develop`, is not this branch's; the branch fixes nothing else to get through and does not edit `.clang-tidy` or `cmake/cppcheck.supp`.
- `elctl init argus` into a temp dir writes `<root>/.nobion/install.toml` and `data/nobion.db`; `elctl doctor` on it passes; `nobion --self-test` passes (`tests/integration/test_install_e2e.cpp` and `tests/setup/install_test.cmake` assert the new names after the rewrite).
- The metrics endpoint exposes only `nobion_*`; `packaging/observability` queries only those (existing `MetricsPackaging` tests).

## 6. Keeping it renamed

`tools/check_names.py`: fails, naming file and line, on a case-insensitive `pychron` in a tracked text file or path that section 3 does not allow (same `name_keep.txt`; in identifiers-only paths it fails only on the tokens listed there). Run by `tools/githooks/pre-push` and ctest `tools.check_names`. Not built: section 9.

## 7. Other branches and worktrees

Unlanded branches with work (12 at `8d3ad1da`, among them `feat/grains` with 30 commits, `feat/systems-t096`, `feat/data-t070`, `feat/extractionline-t093`):

- `feat/core-build-identity` (field diagnostics Task 1, unreviewed WIP `a9693f78`): branch and worktree deleted; field diagnostics restarts at Task 1 after this lands.
- Any other branch, when next worked on: run `tools/rename_to_nobion.py` on its tip, commit `refactor: rename to nobion`, rebase onto `develop` (matching hunks fall away; what is left is real conflict), delete its build tree. `docs/dev_setup.md` gives the steps. This branch touches no other worktree. Script not built: section 9.

## 8. Out of scope

- Company, organisation, bundle-id domain (kept, 3.4).
- Renaming `elctl`.
- Landed specs and plans, `CHANGELOG.md`, applied migrations, a migration rewriting the `pychron:` texts in database functions.
- Renaming other branches (section 7 gives the steps).

## 9. As built

- Rename landed as commits by area, not one generated commit: folders (`01d1f197`), env vars (`02658439`), include root (`eef84880`), namespace (`3dc7636a`), CMake names (`038aa9f2`), metric names (`1a5f41c4`), log and database files (`a82229a9`), identities, formats, messages (`f07a7852`), two findings (`698c8a05`). `tools/rename_to_nobion.py` and `tools/name_keep.txt` never committed. Section 3 exceptions held by hand: remaining `pychron` on `develop` (243 files outside `docs/superpowers` and `CHANGELOG.md`, 2026-11-08) are the section 3 kinds (legacy protocol files, legacy prose, legacy data and fixtures, company, EarthBank vocabulary, applied migrations).
- Built as specified: `cpack_sign.cmake` filter `/Nobion\.app$`; field diagnostics spec and plan in new names (`4209ad8b`, `112e9421`); section 4 deletion of `settings_migration.{hpp,cpp}` and `tests/ui/test_settings_migration.cpp` (2026-11-08, own commit).
- Dropped: `tools/rename_local_state.py`. Owner's Mac already on new names, checked 2026-11-08: `~/Library/Logs/nobion`, `~/Library/Application Support/Nobion`, each install's `<root>/.nobion`, no `PYCHRON_*` env var. Nothing left for it to move.
- Owed, own branch: `tools/check_names.py`, `tools/name_keep.txt`, unittests, pre-push and ctest `tools.check_names` (section 6); `tools/rename_to_nobion.py` for unlanded branches and its `docs/dev_setup.md` steps (section 7). Until then, an unlanded branch is renamed by hand before its rebase.

## Open questions

None. (Company names: owner, 2026-11-06, keep them; section 3.4.)
