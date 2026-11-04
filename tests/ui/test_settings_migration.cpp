// The settings a pychron-ui install left behind, carried over once to nobion
// (nobion app spec, "Settings migration"). Both sides are ini files in a
// temporary directory, never the developer's own preferences.

#include <filesystem>
#include <fstream>
#include <string>

#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "settings_migration.hpp"

namespace fs = std::filesystem;
using pychron::ui::migrate_config_dir;
using pychron::ui::migrate_settings;

namespace {

void write_file(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

class TestSettingsMigration : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;

  [[nodiscard]] QString ini(const char* name) const { return dir_.filePath(QString::fromLatin1(name)); }
  [[nodiscard]] fs::path path(const char* name) const { return fs::path(dir_.filePath(QString::fromLatin1(name)).toStdString()); }

 private slots:
  void copiesEveryKeyIntoEmptySettings() {
    QSettings from(ini("old1.ini"), QSettings::IniFormat);
    from.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
    from.setValue(QStringLiteral("window/geometry"), QByteArray("\x01\x02", 2));
    QSettings to(ini("new1.ini"), QSettings::IniFormat);

    QCOMPARE(migrate_settings(from, to), 2);
    QCOMPARE(to.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
    QCOMPARE(to.value(QStringLiteral("window/geometry")).toByteArray(), QByteArray("\x01\x02", 2));
    QCOMPARE(from.allKeys().size(), 2);
  }

  void leavesSettingsThatHaveAKeyAlone() {
    QSettings from(ini("old2.ini"), QSettings::IniFormat);
    from.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
    QSettings to(ini("new2.ini"), QSettings::IniFormat);
    to.setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));

    QCOMPARE(migrate_settings(from, to), 0);
    QCOMPARE(to.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("light"));
  }

  void copiesNothingFromEmptySettings() {
    const QSettings from(ini("old3.ini"), QSettings::IniFormat);
    QSettings to(ini("new3.ini"), QSettings::IniFormat);
    QCOMPARE(migrate_settings(from, to), 0);
    QVERIFY(to.allKeys().isEmpty());
  }

  void copiesAConfigDirectoryWhenTheTargetIsMissing() {
    write_file(path("old_cfg1") / "presets" / "ideogram.toml", "a = 1\n");
    const auto copied = migrate_config_dir(path("old_cfg1"), path("new_cfg1"));
    QVERIFY(copied.has_value());
    QVERIFY(*copied);
    QCOMPARE(read_file(path("new_cfg1") / "presets" / "ideogram.toml"), std::string("a = 1\n"));
    QVERIFY(fs::exists(path("old_cfg1") / "presets" / "ideogram.toml"));
  }

  void leavesAnExistingConfigDirectoryAlone() {
    write_file(path("old_cfg2") / "presets" / "ideogram.toml", "a = 1\n");
    write_file(path("new_cfg2") / "presets" / "ideogram.toml", "a = 2\n");
    const auto copied = migrate_config_dir(path("old_cfg2"), path("new_cfg2"));
    QVERIFY(copied.has_value());
    QVERIFY(!*copied);
    QCOMPARE(read_file(path("new_cfg2") / "presets" / "ideogram.toml"), std::string("a = 2\n"));
  }

  void copiesNothingWhenThereIsNoOldDirectory() {
    const auto copied = migrate_config_dir(path("no_such_dir"), path("new_cfg3"));
    QVERIFY(copied.has_value());
    QVERIFY(!*copied);
    QVERIFY(!fs::exists(path("new_cfg3")));
  }
};

QTEST_MAIN(TestSettingsMigration)
#include "test_settings_migration.moc"
