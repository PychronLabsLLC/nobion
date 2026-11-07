// ValvePopup: the faceplate a left click on a valve opens (spec
// 2026-11-04-hmi-valves §6): what it shows, the controls it offers, and that
// they reach the line only through the view.

#include <memory>
#include <string>

#include <QPushButton>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "canvas_state.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "ui_fixture.hpp"
#include "valve_popup.hpp"

using namespace nobion;
using nobion::systems::SwitchOp;
using nobion::ui::CanvasView;
using nobion::ui::CoreBridge;
using nobion::ui::ValvePopup;

class TestValvePopup : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_example_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
    view_ = std::make_unique<CanvasView>(*bridge_);
    view_->resize(1100, 800);
    QVERIFY(line_->start().has_value());
    QTRY_VERIFY(bridge_->state().valves.count("B") == 1);
    view_->set_confirm_unlock([](const QString&) { return true; });
    view_->show();
    QVERIFY(QTest::qWaitForWindowExposed(view_.get()));
  }

  void cleanup() {
    line_->stop();
    view_.reset();
    bridge_.reset();
    line_.reset();
  }

  // Selecting opens the faceplate: name, state, history, and the one
  // operation the state allows.
  void selectingShowsTheFaceplate() {
    view_->select("B");
    ValvePopup* popup = view_->valve_popup();
    QVERIFY(popup != nullptr);
    QVERIFY(popup->isVisible());
    const QStringList text = popup->text();
    QVERIFY2(text.first().startsWith(QStringLiteral("B")), qPrintable(text.join('|')));
    QVERIFY2(text.join('\n').contains(QStringLiteral("Closed")), qPrintable(text.join('|')));
    // The tooltip's label-value rows read as plain lines here.
    QVERIFY2(!text.join('\n').contains(QLatin1Char('\t')), qPrintable(text.join('|')));
    QVERIFY2(!text.join('\n').contains(QStringLiteral("State")), qPrintable(text.join('|')));
    QVERIFY2(text.join('\n').contains(QStringLiteral("Actuations: ")), qPrintable(text.join('|')));
    QVERIFY(popup->open_button()->isVisible());
    QVERIFY(popup->open_button()->isEnabled());
    QVERIFY(!popup->close_button()->isVisible());
    QVERIFY(popup->lock_button()->isVisible());
    QCOMPARE(popup->lock_button()->text(), QStringLiteral("Lock"));
    QVERIFY(view_->valve("B")->is_selected());
  }

  // Open sends Open; the faceplate follows the valve to its new state and
  // stays open.
  void openOpensTheValveAndTheFaceplateFollows() {
    view_->select("B");
    ValvePopup* popup = view_->valve_popup();
    popup->open_button()->click();
    QTRY_COMPARE(view_->valve("B")->state(), ValveState::Open);
    QTRY_VERIFY(popup->close_button()->isVisible());
    QVERIFY(!popup->open_button()->isVisible());
    QVERIFY(popup->isVisible());
    popup->close_button()->click();
    QTRY_COMPARE(view_->valve("B")->state(), ValveState::Closed);
  }

  // While the command is in flight the faceplate says so and offers nothing.
  void aCommandInFlightDisablesTheButtons() {
    view_->select("B");
    ValvePopup* popup = view_->valve_popup();
    popup->open_button()->click();
    QVERIFY(view_->valve("B")->is_pending());
    QVERIFY2(popup->text().contains(QStringLiteral("Moving…")), qPrintable(popup->text().join('|')));
    QVERIFY(!popup->close_button()->isEnabled());
    QTRY_VERIFY(!view_->valve("B")->is_pending());
    QTRY_VERIFY(popup->close_button()->isEnabled());
  }

  // Lock locks; a locked valve's operation is disabled and says why; unlock
  // asks first, and a no leaves it locked.
  void lockAndUnlockFromTheFaceplate() {
    view_->select("B");
    ValvePopup* popup = view_->valve_popup();
    popup->lock_button()->click();
    QTRY_VERIFY(line_->is_locked("B"));
    QTRY_COMPARE(popup->lock_button()->text(), QStringLiteral("Unlock"));
    QVERIFY(!popup->open_button()->isEnabled());
    QVERIFY(popup->text().contains(QStringLiteral("Locked: unlock to operate")));

    bool asked = false;
    view_->set_confirm_unlock([&](const QString&) {
      asked = true;
      return false;
    });
    popup->lock_button()->click();
    QVERIFY(asked);
    QVERIFY(line_->is_locked("B"));

    view_->set_confirm_unlock([](const QString&) { return true; });
    popup->lock_button()->click();
    QTRY_VERIFY(!line_->is_locked("B"));
  }

  // A manual valve is told, not driven: its faceplate has no controls; nor
  // has any faceplate of a read-only view.
  void manualAndReadOnlyHaveNoControls() {
    view_->select("M1");
    ValvePopup* popup = view_->valve_popup();
    QVERIFY(popup->isVisible());
    QVERIFY(!popup->open_button()->isVisible());
    QVERIFY(!popup->close_button()->isVisible());
    QVERIFY(!popup->lock_button()->isVisible());
    popup->hide();

    ui::CoreBridgeCanvasState state(*bridge_);
    CanvasView ro(state, nullptr);
    ro.resize(1100, 800);
    ro.show();
    QVERIFY(QTest::qWaitForWindowExposed(&ro));
    ro.select("B");
    QVERIFY(ro.valve_popup()->isVisible());
    QVERIFY(!ro.valve_popup()->open_button()->isVisible());
    QVERIFY(!ro.valve_popup()->lock_button()->isVisible());
  }

  // A rows for each partner its opening waits on, met or not as the state is.
  void interlockRowsSayWhetherTheyAreMet() {
    const auto f = view_->faceplate("A");  // interlocks = ["C", "D"]
    QCOMPARE(f.interlocks.size(), std::size_t{2});
    QCOMPARE(f.interlocks[0].partner, QStringLiteral("C"));
    QVERIFY(f.interlocks[0].must_be_closed);
    QVERIFY(f.interlocks[0].met);

    bridge_->actuate("C", SwitchOp::Open);
    QTRY_COMPARE(view_->valve("C")->state(), ValveState::Open);
    QVERIFY(!view_->faceplate("A").interlocks[0].met);
    view_->select("A");
    QVERIFY(view_->valve_popup()->text().contains(QStringLiteral("Opens only with C closed: not met")));
    QVERIFY(view_->faceplate("B").interlocks.empty() || view_->faceplate("B").interlocks[0].partner != "C");
  }

  // A rejection's reason shows on the faceplate of the valve it hit.
  void aRejectionShowsItsReason() {
    bridge_->actuate("C", SwitchOp::Open);
    QTRY_COMPARE(view_->valve("C")->state(), ValveState::Open);
    view_->select("A");
    ValvePopup* popup = view_->valve_popup();
    popup->open_button()->click();
    QTRY_VERIFY2(popup->text().join('\n').contains(QStringLiteral("Last failure: ")), qPrintable(popup->text().join('|')));
    QVERIFY(popup->text().join('\n').contains(QStringLiteral("interlocked")));
  }

  // Esc closes the faceplate and ends the selection.
  void escapeClosesAndDeselects() {
    view_->select("B");
    ValvePopup* popup = view_->valve_popup();
    QSignalSpy closed(popup, &ValvePopup::closed);
    QTest::keyClick(popup, Qt::Key_Escape);
    QVERIFY(!popup->isVisible());
    QCOMPARE(closed.count(), 1);
    QVERIFY(view_->selected().empty());
    QVERIFY(!view_->valve("B")->is_selected());
  }

  // Selecting another valve moves the one faceplate and the ring.
  void selectingAnotherMovesTheFaceplate() {
    view_->select("B");
    ValvePopup* first = view_->valve_popup();
    view_->select("A");
    QCOMPARE(view_->valve_popup(), first);
    QCOMPARE(first->faceplate().name, std::string("A"));
    QVERIFY(view_->valve("A")->is_selected());
    QVERIFY(!view_->valve("B")->is_selected());
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
  std::unique_ptr<CanvasView> view_;
};

QTEST_MAIN(TestValvePopup)
#include "test_valve_popup.moc"
