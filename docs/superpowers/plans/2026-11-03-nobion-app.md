# Plan: UI app renamed nobion

Spec: `docs/superpowers/specs/2026-11-03-nobion-app-design.md`.

1. Move the tree. `git mv apps/pychron-ui apps/nobion`, rename the packaging
   icon, desktop and resource files, and replace `apps/pychron-ui`,
   `pychron-ui`, `Pychron.app` and the package file names in every live file
   (not `CHANGELOG.md`, not `docs/superpowers`).
2. Settings migration, test first. Add `tests/ui/test_settings_migration.cpp`
   (copies into empty settings; leaves non-empty settings alone; copies a
   config directory only when the target is missing), then
   `apps/nobion/src/settings_migration.{hpp,cpp}` and its entry in the
   UI library's sources.
3. Bundle and packages. Set the bundle name, output name and identifier in
   `cmake/PychronInstall.cmake`; set the package, volume, install directory
   and NSIS names in `cmake/PychronPackaging.cmake`; follow them in
   `.github/workflows/release.yml`.
4. UI text. Program name in the setup wizard, Preferences, installations
   dialog and wordmark; call the migration from `main`.
5. Check. `cmake --build --preset dev-ui`, `python3 tools/quality_check.py`,
   `python3 tools/compiler_sweep.py --build-dir build/dev-ui --changed`,
   `ctest --preset dev-ui`; `cmake --install` of the bundle to a temporary
   prefix shows `Nobion.app/Contents/MacOS/Nobion`.

## Open questions

None.
