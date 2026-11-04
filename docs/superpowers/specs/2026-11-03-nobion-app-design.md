# UI app renamed nobion

Product renamed pychron-cpp → nobion (`20fe272e`). UI program still `pychron-ui`, bundle and installers still `Pychron`. This renames them. Legacy pychron (Python program, its files, its repo) keeps its name, as do `pychron::` namespace, `PYCHRON_` macros, `pychron_*` CMake targets, `libs/` include paths, organisation `PychronLabs` / `Pychron Labs LLC`, and `elctl`.

## What changes

- Source directory `apps/pychron-ui` → `apps/nobion`; executable target `pychron-ui` → `nobion`. Internal targets `pychron_ui_lib`, `pychron::ui`, `pychron_ui_test_*` keep names.
- Every live reference follows: CMake, CI workflows, `AGENTS.md` rows, `tools/`, user and developer docs, comments, `--version` output (`nobion <version>`), error prefix (`nobion: ...`).
- macOS bundle `Pychron.app` → `Nobion.app`: `OUTPUT_NAME Nobion`, `MACOSX_BUNDLE_BUNDLE_NAME Nobion`, identifier `com.pychronlabs.pychron` → `com.pychronlabs.nobion`.
- Packages: `CPACK_PACKAGE_NAME` `nobion`, install directory, DMG volume, NSIS display and Start-menu name `Nobion`, file names `Nobion-<version>-...`, `.deb` `nobion_*.deb`, release artifacts `nobion-<matrix>`.
- Packaging files: `packaging/icons/pychron.{icns,ico,png}` → `nobion.*`, `packaging/linux/pychron.desktop` → `nobion.desktop` (`Name=Nobion`, `Exec=nobion`, `Icon=nobion`), `packaging/windows/pychron.rc` → `nobion.rc`. Icon images unchanged.
- UI text naming this program ("Set up Pychron", "Pychron must reopen ...", About wordmark) says Nobion; wordmark drawn `nobion`.
- Install resource directory `share/pychron` unchanged (`setup::find_resources`, `elctl` read it).

## Settings migration

Qt application name `pychron-ui` → `nobion` moves `QSettings` (macOS `com.pychronlabs.pychron-ui.plist`) and `QStandardPaths::AppConfigLocation` (figure presets). First start of `nobion` carries them over once:

- `migrate_settings(const QSettings& from, QSettings& to)`: copies every key of `from` into `to` only if `to` has no key and `from` has at least one. Returns number copied. `from` never modified.
- `migrate_config_dir(from, to)`: copies `from` directory tree to `to` only if `to` does not exist and `from` is a directory. Existing `to` never touched. Returns whether it copied; failure logged, start continues.
- `main` calls both after `setApplicationName("nobion")`, before anything reads settings. Old location: `QSettings("PychronLabs", "pychron-ui")`; old config dir: `AppConfigLocation` with application name set to `pychron-ui` for one call, then restored.
- Old settings and directory left in place (downgrade still works).

Files: `apps/nobion/src/settings_migration.{hpp,cpp}`, test `tests/ui/test_settings_migration.cpp` (ini-format `QSettings` in temp files).

## Out of scope

- Renaming `elctl`, `pychron::` namespace, `PYCHRON_*` macros and env vars, `pychron_*` targets, `share/pychron`, `.pychron` data files, organisation name.
- Dated specs and plans under `docs/superpowers`, `CHANGELOG.md`.
- macOS camera permission: granted per bundle identifier, so asked again once under `com.pychronlabs.nobion`. Not migratable.
- New icon artwork.

## Open questions

None.
