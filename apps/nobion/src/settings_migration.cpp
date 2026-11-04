#include "settings_migration.hpp"

#include <string>
#include <system_error>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "pychron/core/process_log.hpp"

namespace pychron::ui {

namespace fs = std::filesystem;

int migrate_settings(const QSettings& from, QSettings& to) {
  if (!to.allKeys().isEmpty()) return 0;
  const QStringList keys = from.allKeys();
  for (const QString& key : keys) to.setValue(key, from.value(key));
  to.sync();
  return static_cast<int>(keys.size());
}

Result<bool> migrate_config_dir(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (fs::exists(to, ec) || !fs::is_directory(from, ec)) return false;
  fs::create_directories(to.parent_path(), ec);
  fs::copy(from, to, fs::copy_options::recursive, ec);
  if (ec) return fail(ErrorKind::Io, "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message());
  return true;
}

void migrate_legacy_application_settings() {
  const QString current = QCoreApplication::applicationName();
  const QString legacy = QStringLiteral("pychron-ui");
  if (current == legacy) return;

  const QSettings from(QCoreApplication::organizationName(), legacy);
  QSettings to;
  if (const int copied = migrate_settings(from, to); copied > 0)
    process_logger("nobion").info("settings: " + std::to_string(copied) + " carried over from pychron-ui");

  // AppConfigLocation is named after the application: ask once under the old name.
  QCoreApplication::setApplicationName(legacy);
  const fs::path old_dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString();
  QCoreApplication::setApplicationName(current);
  const fs::path new_dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation).toStdString();
  if (old_dir == new_dir) return;
  const auto copied = migrate_config_dir(old_dir, new_dir);
  if (!copied)
    process_logger("nobion").warn("settings: " + copied.error().what);
  else if (*copied)
    process_logger("nobion").info("settings: " + old_dir.string() + " carried over to " + new_dir.string());
}

}  // namespace pychron::ui
