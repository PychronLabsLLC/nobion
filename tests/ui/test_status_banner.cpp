// The status banner and the alarm strip (spec 2026-11-05-hmi-banner): what
// they say, that they count what the Alarms dock and the health bar hold,
// and that the main window puts them above the canvas.

#include <chrono>
#include <memory>

#include <QLabel>
#include <QSignalSpy>
#include <QToolButton>
#include <QtTest/QtTest>

#include "alarm_dock.hpp"
#include "experiment_bridge.hpp"
#include "experiment_fixture.hpp"
#include "health_bar.hpp"
#include "main_window.hpp"
#include "nobion/core/virtual_clock.hpp"
#include "status_banner.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using pychron::ui::AlarmDock;
using pychron::ui::AlarmStrip;
using pychron::ui::HealthBar;
using pychron::ui::StatusBanner;
namespace exec = pychron::experiment::executor;

namespace {

QDateTime at(int h, int m, int s) { return QDateTime(QDate(2026, 11, 5), QTime(h, m, s)); }

}  // namespace

class TestStatusBanner : public QObject {
  Q_OBJECT

 private slots:
  // The dock's standing alarms, most pressing first: Critical, then Warning,
  // then Info, newest first within each; it says when they change.
  void alarmDockRowsAreMostPressingFirst() {
    QDateTime now = at(13, 0, 0);
    AlarmDock dock(nullptr, [&now] { return now; });
    QSignalSpy changed(&dock, &AlarmDock::changed);
    dock.add_alarm(Alarm{"heater1", AlarmSeverity::Warning, "read failed", {}});
    now = now.addSecs(5);
    dock.add_alarm(Alarm{"note", AlarmSeverity::Info, "fyi", {}});
    now = now.addSecs(5);
    dock.add_alarm(Alarm{"IG1", AlarmSeverity::Critical, "high", {}});
    now = now.addSecs(5);
    dock.add_alarm(Alarm{"cryo", AlarmSeverity::Warning, "drift", {}});
    QCOMPARE(changed.count(), 4);

    const auto rows = dock.rows();
    QCOMPARE(rows.size(), std::size_t{4});
    QCOMPARE(rows[0].source, std::string("IG1"));
    QCOMPARE(rows[1].source, std::string("cryo"));
    QCOMPARE(rows[2].source, std::string("heater1"));
    QCOMPARE(rows[3].source, std::string("note"));
    QCOMPARE(rows[0].time, at(13, 0, 10));
    QCOMPARE(rows[0].message, std::string("high"));

    dock.tree()->topLevelItem(0)->setSelected(true);
    QCOMPARE(dock.acknowledge_selected(), 1);
    QCOMPARE(changed.count(), 5);
    dock.acknowledge_all();
    QCOMPARE(changed.count(), 6);
    QVERIFY(dock.rows().empty());
  }

  // Two alarms in the same second (or simulated instant) go by raise order;
  // a repeat of what a row says is not a change.
  void alarmDockTiesGoByRaiseOrder() {
    AlarmDock dock(nullptr, [] { return at(13, 0, 0); });  // every alarm in the same second
    QSignalSpy changed(&dock, &AlarmDock::changed);
    dock.add_alarm(Alarm{"a", AlarmSeverity::Warning, "one", {}});
    dock.add_alarm(Alarm{"b", AlarmSeverity::Warning, "two", {}});
    dock.add_alarm(Alarm{"c", AlarmSeverity::Warning, "three", {}});
    QCOMPARE(dock.rows().front().source, std::string("c"));
    dock.add_alarm(Alarm{"a", AlarmSeverity::Warning, "one", {}});  // the same again
    QCOMPARE(changed.count(), 3);
    QCOMPARE(dock.rows().front().source, std::string("c"));
    dock.add_alarm(Alarm{"a", AlarmSeverity::Warning, "one, worse", {}});  // changed: raised anew
    QCOMPARE(changed.count(), 4);
    QCOMPARE(dock.rows().front().source, std::string("a"));
    QCOMPARE(dock.rows().front().message, std::string("one, worse"));
  }

  // The health bar counts its chips by status and says when a count moves,
  // not on its age ticks.
  void healthBarSummaryCountsTheChips() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    QSignalSpy changed(&bar, &HealthBar::changed);
    bar.seed({"a", "b", "c"});
    QCOMPARE(bar.summary(), (HealthBar::Summary{0, 0, 0, 3}));
    QCOMPARE(changed.count(), 1);
    bar.update_health(TransportHealth{"a", true, 0, "", now});
    bar.update_health(TransportHealth{"b", true, 2, "timeout", now});
    bar.update_health(TransportHealth{"c", false, 0, "", now});
    QCOMPARE(bar.summary(), (HealthBar::Summary{1, 1, 1, 0}));
    QCOMPARE(changed.count(), 4);
    bar.update_health(TransportHealth{"a", true, 0, "", now});  // no change
    bar.refresh();
    QCOMPARE(changed.count(), 4);
  }

  void modeSaysLiveOrSimulated() {
    StatusBanner banner([] { return at(13, 41, 7); });
    QCOMPARE(banner.mode_text(), QStringLiteral("LIVE"));
    banner.set_mode({true, 0});
    QCOMPARE(banner.mode_text(), QStringLiteral("SIMULATED"));
    banner.set_mode({true, 50});
    QCOMPARE(banner.mode_text(), QStringLiteral("SIMULATED x50"));
    banner.set_mode({false, 0});
    QCOMPARE(banner.mode_text(), QStringLiteral("LIVE"));
  }

  // The clock is the one it is given (the line's), read each tick; it
  // keeps one width, so the fields beside it stay put as it counts.
  void theClockReadsTheLinesClock() {
    QDateTime now = at(13, 41, 7);
    StatusBanner banner([&now] { return now; });
    banner.resize(900, StatusBanner::kHeight);
    banner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&banner));
    QCOMPARE(banner.clock_text(), QStringLiteral("13:41:07"));
    auto* clock = banner.findChild<QLabel*>(QStringLiteral("bannerClock"));
    const QRect before = banner.alarms_button()->geometry();
    const int width = clock->width();
    for (const QTime t : {QTime(11, 11, 11), QTime(20, 8, 59), QTime(0, 0, 0)}) {
      now = QDateTime(QDate(2026, 11, 5), t);
      banner.tick();
      QCoreApplication::processEvents();
      QCOMPARE(clock->width(), width);
      QCOMPARE(banner.alarms_button()->geometry(), before);
      QVERIFY(clock->fontMetrics().horizontalAdvance(clock->text()) <= clock->contentsRect().width());
    }
    QCOMPARE(banner.clock_text(), QStringLiteral("00:00:00"));
  }

  // No field is drawn over another, whatever the executor says.
  void noFieldOverlapsAnother() {
    StatusBanner banner([] { return at(13, 41, 7); });
    banner.set_mode({true, 50});
    banner.set_line(QStringLiteral("example"));
    banner.set_session(false);
    banner.resize(900, StatusBanner::kHeight);
    banner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&banner));
    QStringList names{QStringLiteral("bannerMode"), QStringLiteral("bannerLine"), QStringLiteral("bannerExecutor"),
                      QStringLiteral("bannerAlarms"), QStringLiteral("bannerHealth"), QStringLiteral("bannerClock")};
    QList<QRect> rects;
    for (const auto& name : names) rects += banner.findChild<QWidget*>(name)->geometry();
    for (int i = 0; i + 1 < rects.size(); ++i) {
      QVERIFY2(rects[i].right() < rects[i + 1].left(), qPrintable(names[i] + QStringLiteral(" / ") + names[i + 1]));
    }
    auto* line = banner.findChild<QLabel*>(QStringLiteral("bannerLine"));
    QVERIFY(line->fontMetrics().horizontalAdvance(line->text()) <= line->contentsRect().width());
  }

  // A long name and a long hold reason, in a narrow band: still nothing
  // drawn over anything else, and the band never asks the window to be
  // wider than it is.
  void aNarrowBandWithLongTextsStillDoesNotOverlap() {
    StatusBanner banner([] { return at(13, 41, 7); });
    banner.set_mode({true, 50});
    banner.set_line(QStringLiteral("NMGRL Felix extraction line, the long name"));
    banner.set_session(true);
    banner.on_state({exec::ExecutorState::Running, exec::ExecutorState::Holding, {}});
    exec::ExecutorHolding hold;
    hold.reason = "waiting for valve_bus, cg_controller and gauge_net to come back";
    banner.on_holding(hold);
    banner.set_alarms({{"IG1", AlarmSeverity::Critical, "high", {}}, {"cryo", AlarmSeverity::Warning, "drift", {}}});
    QVERIFY2(banner.minimumSizeHint().width() < 640, qPrintable(QString::number(banner.minimumSizeHint().width())));
    for (const int width : {1200, 640}) {
      banner.resize(width, StatusBanner::kHeight);
      banner.show();
      QVERIFY(QTest::qWaitForWindowExposed(&banner));
      QCoreApplication::processEvents();
      QStringList names{QStringLiteral("bannerMode"), QStringLiteral("bannerLine"), QStringLiteral("bannerExecutor"),
                        QStringLiteral("bannerAlarms"), QStringLiteral("bannerHealth"), QStringLiteral("bannerClock")};
      for (int i = 0; i + 1 < names.size(); ++i) {
        const QRect a = banner.findChild<QWidget*>(names[i])->geometry();
        const QRect b = banner.findChild<QWidget*>(names[i + 1])->geometry();
        QVERIFY2(a.right() < b.left(), qPrintable(QStringLiteral("%1 / %2 at %3").arg(names[i], names[i + 1]).arg(width)));
      }
      // The clock and the alarm counts are never squeezed.
      auto* clock = banner.findChild<QLabel*>(QStringLiteral("bannerClock"));
      QVERIFY(clock->fontMetrics().horizontalAdvance(clock->text()) <= clock->contentsRect().width());
    }
    QCOMPARE(banner.findChild<QLabel*>(QStringLiteral("bannerLine"))->toolTip(),
             QStringLiteral("NMGRL Felix extraction line, the long name"));
  }

  // The executor field follows the executor's events.
  void executorTextFollowsTheExecutor() {
    StatusBanner banner([] { return at(0, 0, 0); });
    QCOMPARE(banner.executor_text(), QStringLiteral("No experiment session"));
    banner.set_session(true);
    QCOMPARE(banner.executor_text(), QStringLiteral("Idle"));
    banner.on_state({exec::ExecutorState::Idle, exec::ExecutorState::Preparing, {}});
    QCOMPARE(banner.executor_text(), QStringLiteral("Preparing queue"));
    banner.on_state({exec::ExecutorState::Preparing, exec::ExecutorState::Running, {}});
    exec::RunStarted run;
    run.identifier = "66120";
    run.name = "66120-01A";
    banner.on_run_started(run);
    QCOMPARE(banner.executor_text(), QStringLiteral("Running 66120-01A"));
    exec::ExecutorHolding hold;
    hold.reason = "valve_bus down";
    banner.on_holding(hold);
    banner.on_state({exec::ExecutorState::Running, exec::ExecutorState::Holding, {}});
    QCOMPARE(banner.executor_text(), QStringLiteral("Holding: valve_bus down"));
    banner.on_state({exec::ExecutorState::Holding, exec::ExecutorState::StoppingAtBoundary, {}});
    QCOMPARE(banner.executor_text(), QStringLiteral("Stopping after 66120-01A"));
    banner.on_state({exec::ExecutorState::StoppingAtBoundary, exec::ExecutorState::Idle, {}});
    QCOMPARE(banner.executor_text(), QStringLiteral("Idle"));
    banner.set_session(false);
    QCOMPARE(banner.executor_text(), QStringLiteral("No experiment session"));

    QCOMPARE(StatusBanner::executor_text(exec::ExecutorState::Aborting, {}, {}), QStringLiteral("Aborting"));
    QCOMPARE(StatusBanner::executor_text(exec::ExecutorState::Cancelling, {}, {}), QStringLiteral("Cancelling"));
    QCOMPARE(StatusBanner::executor_text(exec::ExecutorState::Finalizing, {}, {}), QStringLiteral("Finishing queue"));
  }

  // Counts Critical and Warning; Info is the dock's alone.
  void alarmCountsLeaveOutInfo() {
    StatusBanner banner([] { return at(0, 0, 0); });
    QCOMPARE(banner.alarms_text(), QStringLiteral("No alarms"));
    banner.set_alarms({{"note", AlarmSeverity::Info, "fyi", {}}});
    QCOMPARE(banner.alarms_text(), QStringLiteral("No alarms"));
    banner.set_alarms({{"IG1", AlarmSeverity::Critical, "high", {}},
                       {"IG2", AlarmSeverity::Critical, "high", {}},
                       {"cryo", AlarmSeverity::Warning, "drift", {}},
                       {"note", AlarmSeverity::Info, "fyi", {}}});
    QCOMPARE(banner.alarms_text(), QStringLiteral("2 critical · 1 warning"));
    QSignalSpy clicked(&banner, &StatusBanner::alarms_clicked);
    banner.alarms_button()->click();
    QCOMPARE(clicked.count(), 1);
  }

  void healthTextNamesWhatIsWrong() {
    StatusBanner banner([] { return at(0, 0, 0); });
    banner.set_health({5, 0, 0, 0});
    QCOMPARE(banner.health_text(), QStringLiteral("Connections ok (5)"));
    banner.set_health({3, 1, 1, 0});
    QCOMPARE(banner.health_text(), QStringLiteral("1 down · 1 degraded"));
    banner.set_health({0, 0, 0, 2});
    QCOMPARE(banner.health_text(), QStringLiteral("2 unknown"));
  }

  // Hidden with nothing but Info; the three most pressing, then "+N more".
  void theStripShowsTheMostPressingThree() {
    AlarmStrip strip;
    strip.set_alarms({});
    QVERIFY(strip.isHidden());
    strip.set_alarms({{"note", AlarmSeverity::Info, "fyi", at(1, 0, 0)}});
    QVERIFY(strip.isHidden());
    strip.set_alarms({{"IG1", AlarmSeverity::Critical, "high", at(13, 41, 7)},
                      {"cryo", AlarmSeverity::Warning, "drift", at(13, 40, 0)},
                      {"heater1", AlarmSeverity::Warning, "read failed", at(13, 39, 0)},
                      {"heater2", AlarmSeverity::Warning, "read failed", at(13, 38, 0)},
                      {"heater3", AlarmSeverity::Warning, "read failed", at(13, 37, 0)},
                      {"note", AlarmSeverity::Info, "fyi", at(13, 42, 0)}});
    QVERIFY(!strip.isHidden());
    const QStringList lines = strip.lines();
    QCOMPARE(lines.size(), 4);
    QCOMPARE(lines[0], QStringLiteral("CRITICAL  IG1  high  13:41:07"));
    QVERIFY(lines[1].startsWith(QStringLiteral("WARNING  cryo")));
    QVERIFY(lines[2].startsWith(QStringLiteral("WARNING  heater1")));
    QCOMPARE(lines[3], QStringLiteral("+2 more"));

    QSignalSpy clicked(&strip, &AlarmStrip::clicked);
    strip.resize(600, 80);
    QTest::mouseClick(&strip, Qt::LeftButton, {}, QPoint(300, 10));
    QCOMPARE(clicked.count(), 1);
  }

  // In the main window: banner, strip, canvas, top to bottom; an alarm on
  // the line reaches the dock, then the banner and the strip; a click shows
  // the dock; acknowledging empties both.
  void theMainWindowShowsThemAboveTheCanvas() {
    auto line = ui::test::make_example_line();
    {
      ui::MainWindow window(*line);
      window.set_line_mode({true, 0});
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      QVERIFY(line->start().has_value());
      StatusBanner* banner = window.status_banner();
      AlarmStrip* strip = window.alarm_strip();
      QVERIFY(banner != nullptr && strip != nullptr);
      QCOMPARE(banner->mode_text(), QStringLiteral("SIMULATED"));
      QCOMPARE(banner->line_text(), QString::fromStdString(line->config().system.name));
      QCOMPARE(banner->executor_text(), QStringLiteral("No experiment session"));
      QVERIFY(banner->geometry().bottom() < window.canvas_view()->geometry().top());
      QVERIFY(strip->isHidden());
      QVERIFY2(banner->minimumSizeHint().width() < 700, qPrintable(QString::number(banner->minimumSizeHint().width())));

      line->bus().publish(Alarm{"IG1", AlarmSeverity::Critical, "high", line->clock().now()});
      QTRY_COMPARE(banner->alarms_text(), QStringLiteral("1 critical"));
      QTRY_VERIFY(strip->isVisible());
      QVERIFY(strip->lines().front().startsWith(QStringLiteral("CRITICAL  IG1  high")));
      QVERIFY(strip->geometry().bottom() < window.canvas_view()->geometry().top());

      window.alarm_dock()->hide();
      banner->alarms_button()->click();
      QVERIFY(window.alarm_dock()->isVisible());
      window.alarm_dock()->hide();
      QTest::mouseClick(strip, Qt::LeftButton, {}, QPoint(20, 5));
      QVERIFY(window.alarm_dock()->isVisible());

      window.alarm_dock()->acknowledge_all();
      QCOMPARE(banner->alarms_text(), QStringLiteral("No alarms"));
      QVERIFY(strip->isHidden());

      // Every transport of the example line is counted, and a transport's
      // report moves the banner.
      const auto s = window.health_bar()->summary();
      QCOMPARE(static_cast<std::size_t>(s.ok + s.degraded + s.down + s.unknown), line->config().transports.size());
      line->bus().publish(TransportHealth{"valve_bus", false, 1, "gone", line->clock().now()});
      QTRY_VERIFY2(banner->health_text().startsWith(QStringLiteral("1 down")), qPrintable(banner->health_text()));

      // Help > Canvas Legend opens the canvas's legend, as its button does.
      QVERIFY(window.legend_action() != nullptr);
      window.legend_action()->trigger();
      QVERIFY(window.canvas_view()->legend() != nullptr && window.canvas_view()->legend()->isVisible());
      QVERIFY(window.canvas_view()->legend_button()->isChecked());
      line->stop();
    }
  }

  // The main window gives the banner the executor's events, and a session
  // swapped or gone resets it and leaves the old bridge unheard.
  void theMainWindowFollowsTheExecutor() {
    ui::test::SimLab sim;
    ui::MainWindow window(*sim.line);
    StatusBanner* banner = window.status_banner();
    QCOMPARE(banner->executor_text(), QStringLiteral("No experiment session"));
    auto first = std::make_unique<ui::ExperimentBridge>(*sim.session, sim.line->bus());
    window.set_experiment(first.get(), true);
    QCOMPARE(banner->executor_text(), QStringLiteral("Idle"));
    emit first->executorStateChanged({exec::ExecutorState::Idle, exec::ExecutorState::Running, {}});
    exec::RunStarted run;
    run.name = "66120-01A";
    emit first->runStarted(run);
    QCOMPARE(banner->executor_text(), QStringLiteral("Running 66120-01A"));
    emit first->executorStateChanged({exec::ExecutorState::Running, exec::ExecutorState::Holding, {}});
    exec::ExecutorHolding hold;
    hold.reason = "valve_bus down";
    emit first->executorHolding(hold);
    QCOMPARE(banner->executor_text(), QStringLiteral("Holding: valve_bus down"));

    ui::ExperimentBridge second(*sim.session, sim.line->bus());
    window.set_experiment(&second, true);
    QCOMPARE(banner->executor_text(), QStringLiteral("Idle"));
    emit first->executorStateChanged({exec::ExecutorState::Idle, exec::ExecutorState::Aborting, {}});
    QCOMPARE(banner->executor_text(), QStringLiteral("Idle"));  // the old bridge is not heard
    window.set_experiment(nullptr, true);
    QCOMPARE(banner->executor_text(), QStringLiteral("No experiment session"));
  }
};

QTEST_MAIN(TestStatusBanner)
#include "test_status_banner.moc"
