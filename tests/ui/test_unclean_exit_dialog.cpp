// UncleanExitDialog: says which build ended without a clean exit, and its
// Collect Diagnostics button asks for the collection (Task 19 connects it).

#include <QtTest/QtTest>

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include "nobion/core/build_info.hpp"
#include "nobion/core/log_hub.hpp"
#include "unclean_exit_dialog.hpp"

class TestUncleanExitDialog : public QObject {
  Q_OBJECT

 private slots:
  void uncleanExitDialogOffersCollect() {
    nobion::PreviousSession previous;
    previous.program = "nobion";
    previous.build_id = nobion::build_id();
    previous.started = "2026-11-06T14:03:22Z";
    previous.crash_file = std::filesystem::path("crash/nobion-4242.txt");

    nobion::ui::UncleanExitDialog dialog(previous, nullptr);
    dialog.show();
    const auto* label = dialog.findChild<QLabel*>(QStringLiteral("uncleanExitText"));
    QVERIFY(label != nullptr);
    QVERIFY(label->text().contains(QString::fromStdString(nobion::build_id())));
    QVERIFY(label->text().contains(QStringLiteral("2026-11-06T14:03:22Z")));
    QVERIFY(label->text().contains(QStringLiteral("nobion-4242.txt")));

    auto* collect = dialog.findChild<QPushButton*>(QStringLiteral("collectDiagnosticsButton"));
    QVERIFY(collect != nullptr);
    QCOMPARE(collect->text(), QStringLiteral("Collect Diagnostics…"));
    QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("dismissButton")) != nullptr);

    QSignalSpy spy(&dialog, &nobion::ui::UncleanExitDialog::collectRequested);
    collect->click();
    QCOMPARE(spy.count(), 1);
  }

  // Programs share a log folder: the dialog names the program that did not
  // close cleanly, which can be elctl.
  void namesTheSentinelsProgram() {
    nobion::PreviousSession previous;
    previous.program = "elctl";
    previous.build_id = "b";
    previous.started = "t";
    nobion::ui::UncleanExitDialog dialog(previous, nullptr);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("elctl did not close cleanly last time"));
    const auto* label = dialog.findChild<QLabel*>(QStringLiteral("uncleanExitText"));
    QVERIFY(label != nullptr);
    QVERIFY(label->text().contains(QStringLiteral("previous elctl session")));
  }

  void dismissEmitsNothing() {
    nobion::PreviousSession previous;
    previous.build_id = "b";
    previous.started = "t";
    nobion::ui::UncleanExitDialog dialog(previous, nullptr);
    dialog.show();
    QSignalSpy spy(&dialog, &nobion::ui::UncleanExitDialog::collectRequested);
    dialog.findChild<QPushButton*>(QStringLiteral("dismissButton"))->click();
    QCOMPARE(spy.count(), 0);
    QVERIFY(!dialog.isVisible());
  }
};

QTEST_MAIN(TestUncleanExitDialog)
#include "test_unclean_exit_dialog.moc"
