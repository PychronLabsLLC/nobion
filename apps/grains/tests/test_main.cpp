// Two QTEST_MAIN cannot share a binary: run each class in turn.
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "test_grain_overlay.hpp"
#include "test_grains_window.hpp"

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("pychron-test"));
  QCoreApplication::setApplicationName(QStringLiteral("grains-test"));
  // Settings in a folder of their own: the user's are not read or written.
  const QTemporaryDir settings_dir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());

  int failed = 0;
  {
    pychron::grains::test::GrainOverlayTest overlay;
    failed |= QTest::qExec(&overlay, argc, argv);
  }
  {
    pychron::grains::test::GrainsWindowTest window;
    failed |= QTest::qExec(&window, argc, argv);
  }
  return failed;
}
