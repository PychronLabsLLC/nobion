#pragma once

// Carrying the settings of a pychron-ui install over to nobion, once (nobion
// app spec, "Settings migration"). The program was called pychron-ui, and Qt
// files its QSettings and its AppConfigLocation under the application name,
// so the first start under the new name would otherwise find nothing.
// What was left under the old name is never changed: an older build still
// reads it.

#include <filesystem>

#include "pychron/core/error.hpp"

class QSettings;

namespace pychron::ui {

// Copies every key of `from` into `to` when `to` has none; returns how many
// were copied (0 when `to` already had a key or `from` was empty).
int migrate_settings(const QSettings& from, QSettings& to);

// Copies the directory `from`, with everything beneath it, to `to` when `to`
// does not exist and `from` is a directory. Returns whether it copied.
[[nodiscard]] Result<bool> migrate_config_dir(const std::filesystem::path& from, const std::filesystem::path& to);

// Both of the above for this program, from the pychron-ui application name to
// the current one; called once at start, after the application name is set
// and before anything reads settings. A failure is logged, not fatal.
void migrate_legacy_application_settings();

}  // namespace pychron::ui
