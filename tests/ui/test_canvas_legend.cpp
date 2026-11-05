// The canvas legend (spec 2026-11-05-canvas-legend): the Legend button on
// every canvas, the window it opens, and that each row is drawn by the
// canvas's own items.

#include <memory>
#include <set>
#include <string>

#include <QImage>
#include <QPointer>
#include <QSignalSpy>
#include <QToolButton>
#include <QtTest/QtTest>

#include "canvas_legend.hpp"
#include "canvas_state.hpp"
#include "canvas_view.hpp"
#include "core_bridge.hpp"
#include "theme.hpp"
#include "ui_fixture.hpp"

using namespace pychron;
using pychron::ui::CanvasLegend;
using pychron::ui::CanvasView;
using pychron::ui::CoreBridge;
using pychron::ui::LegendEntry;
using pychron::ui::LegendInputs;

namespace {

QColor pixel(const QPixmap& pixmap, QPointF logical) {
  const QImage image = pixmap.toImage();
  const qreal dpr = pixmap.devicePixelRatio();
  return image.pixelColor((logical * dpr).toPoint());
}

QPointF centre(const QPixmap& pixmap) {
  return {pixmap.width() / pixmap.devicePixelRatio() / 2, pixmap.height() / pixmap.devicePixelRatio() / 2};
}

bool near(const QColor& a, const QColor& b, int tol = 12) {
  return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
         std::abs(a.blue() - b.blue()) <= tol;
}

bool contains_color(const QPixmap& pixmap, const QColor& color) {
  const QImage image = pixmap.toImage();
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (near(image.pixelColor(x, y), color, 6)) return true;
    }
  }
  return false;
}

const LegendEntry* find(const std::vector<LegendEntry>& entries, const QString& key) {
  for (const auto& e : entries) {
    if (e.key == key) return &e;
  }
  return nullptr;
}

}  // namespace

class TestCanvasLegend : public QObject {
  Q_OBJECT

 private slots:
  void init() {
    line_ = ui::test::make_example_line();
    bridge_ = std::make_unique<CoreBridge>(*line_);
    view_ = std::make_unique<CanvasView>(*bridge_);
    view_->resize(1100, 800);
    QVERIFY(line_->start().has_value());
    view_->show();
    QVERIFY(QTest::qWaitForWindowExposed(view_.get()));
  }

  void cleanup() {
    line_->stop();
    view_.reset();
    bridge_.reset();
    line_.reset();
  }

  // Top right of the canvas, inside it; nothing open until it is pressed.
  void theButtonSitsTopRight() {
    QToolButton* button = view_->legend_button();
    QVERIFY(button != nullptr);
    QVERIFY(button->isVisible());
    QCOMPARE(button->parentWidget(), view_->viewport());
    const QRect r = button->geometry();
    QVERIFY(view_->viewport()->rect().contains(r));
    QVERIFY2(r.right() > view_->viewport()->width() - 40, qPrintable(QString::number(r.right())));
    QVERIFY(r.top() < 40);
    QVERIFY(view_->legend() == nullptr);
    // A framed button, not text on the drawing.
    QVERIFY(button->property("outlined").toBool());

    view_->resize(700, 500);
    QTRY_VERIFY(button->geometry().right() > view_->viewport()->width() - 40);
  }

  // The button opens the legend and is checked while it shows; closing the
  // legend unchecks it, and the button closes it too.
  void theButtonTogglesTheLegend() {
    QToolButton* button = view_->legend_button();
    button->click();
    CanvasLegend* legend = view_->legend();
    QVERIFY(legend != nullptr);
    QVERIFY(legend->isVisible());
    QVERIFY(button->isChecked());
    QVERIFY(!legend->entries().empty());

    QCOMPARE(legend->windowTitle(), QStringLiteral("Canvas legend"));
    QVERIFY(legend->isWindow());
    QCOMPARE(legend->windowType(), Qt::Tool);
    QVERIFY(!legend->isModal());

    QSignalSpy shown(legend, &CanvasLegend::shown_changed);
    QTest::keyClick(legend, Qt::Key_Escape);
    QVERIFY(!legend->isVisible());
    QCOMPARE(shown.count(), 1);
    QCOMPARE(shown.front().front().toBool(), false);
    QVERIFY(!button->isChecked());

    button->click();
    QVERIFY(legend->isVisible());
    QCOMPARE(view_->legend(), legend);
    button->click();
    QVERIFY(!legend->isVisible());
    QVERIFY(!button->isChecked());
  }

  // Shown again by anything but the button (its window coming back), the
  // legend checks the button again.
  void theButtonFollowsTheWindow() {
    view_->show_legend(true);
    CanvasLegend* legend = view_->legend();
    legend->hide();
    QVERIFY(!view_->legend_button()->isChecked());
    legend->show();
    QVERIFY(view_->legend_button()->isChecked());
  }

  // A view closed with its legend open takes it down cleanly.
  void aViewGoesWithItsLegendOpen() {
    view_->show_legend(true);
    QPointer<CanvasLegend> legend = view_->legend();
    QVERIFY(legend->isVisible());
    view_.reset();
    QVERIFY(legend.isNull());
  }

  // A rehearsal's read-only canvas has the legend too.
  void aReadOnlyViewHasTheLegend() {
    ui::CoreBridgeCanvasState state(*bridge_);
    CanvasView ro(state, nullptr);
    ro.resize(800, 600);
    ro.show();
    QVERIFY(QTest::qWaitForWindowExposed(&ro));
    QVERIFY(ro.legend_button()->isVisible());
    ro.legend_button()->click();
    QVERIFY(ro.legend() != nullptr && ro.legend()->isVisible());
  }

  // Every symbol the canvas draws has its row.
  void everySymbolHasARow() {
    const auto entries = ui::legend_entries(view_->legend_inputs());
    for (const char* key :
         {"valve.actuated", "valve.manual", "valve.switch", "valve.open", "valve.closed", "valve.unknown",
          "valve.pending", "valve.selected", "valve.locked", "valve.failed", "gauge.reading", "gauge.alarm",
          "gauge.stale", "gauge.unwired", "stage.volume", "stage.spectrometer", "stage.quadrupole", "stage.laser", "stage.turbo",
          "stage.getter", "stage.ion_pump", "stage.cryo", "stage.roughing_pump", "pipe", "section.none",
          "section.rule"}) {
      const LegendEntry* e = find(entries, QString::fromLatin1(key));
      QVERIFY2(e != nullptr, key);
      QVERIFY2(!e->text.isEmpty(), key);
      QVERIFY2(QString::fromLatin1(key) == QStringLiteral("section.rule") || !e->glyph.isNull(), key);
    }
    std::set<QString> keys;
    for (const auto& e : entries) QVERIFY2(keys.insert(e.key).second, qPrintable(e.key));
  }

  // The section colours are this canvas's sources, best first, each in the
  // colour the canvas paints it.
  void sourceRowsAreTheCanvasSources() {
    const LegendInputs in = view_->legend_inputs();
    QVERIFY(!in.sources.empty());
    const auto entries = ui::legend_entries(in);
    for (const auto& s : in.sources) {
      const LegendEntry* e = find(entries, QStringLiteral("source.") + s.key);
      QVERIFY2(e != nullptr, qPrintable(s.key));
      QVERIFY(e->text.startsWith(s.label));
      QVERIFY2(near(pixel(e->glyph, centre(e->glyph)), s.color, 4), qPrintable(s.key));
      // A source alone is its own region: the canvas paints it this colour.
      const ui::StageItem* item = view_->stage(s.key.toStdString());
      QVERIFY(item != nullptr);
      QCOMPARE(item->region_color(), s.color);
    }
    // Best first: the pumps, one row for the one colour they share.
    const QString pumps = in.sources.front().label;
    QVERIFY2(pumps.contains(QStringLiteral("turbo2")) && pumps.contains(QStringLiteral("rough")) &&
                 pumps.endsWith(QStringLiteral("(pump)")),
             qPrintable(pumps));
    // A colour has one row.
    for (std::size_t i = 0; i < in.sources.size(); ++i) {
      for (std::size_t j = i + 1; j < in.sources.size(); ++j) QVERIFY(in.sources[i].color != in.sources[j].color);
    }
  }

  // The open and closed rows are filled as the style fills a valve.
  void valveRowsFollowTheStyle() {
    LegendInputs in = view_->legend_inputs();
    in.valve_style = canvas::ValveStyle::Hmi;
    in.open_valve_color = canvas::OpenValveColor::Green;
    auto entries = ui::legend_entries(in);
    const QPixmap open = find(entries, QStringLiteral("valve.open"))->glyph;
    // Inside the bow-tie's left half, clear of the stem; and a corner of the
    // box, clear of the glyph and the padlock.
    const QPointF tie = centre(open) + QPointF(-5, 0);
    const QPointF corner = centre(open) + QPointF(-12, 12);
    QVERIFY(near(pixel(open, tie), ui::theme().valve_open));
    QVERIFY(near(pixel(open, corner), in.sources.front().color));  // hmi: the section's colour
    const QPixmap shut = find(entries, QStringLiteral("valve.closed"))->glyph;
    QVERIFY(near(pixel(shut, tie), ui::theme().valve_closed));
    QVERIFY(near(pixel(shut, corner), ui::theme().inactive));
    QVERIFY(find(entries, QStringLiteral("valve.closed"))->text.contains(QStringLiteral("black glyph")));

    in.valve_style = canvas::ValveStyle::Redgreen;
    entries = ui::legend_entries(in);
    const QPixmap red_green = find(entries, QStringLiteral("valve.open"))->glyph;
    QVERIFY(near(pixel(red_green, tie), ui::theme().valve_open));
    QVERIFY(near(pixel(red_green, corner), ui::theme().ok));
    const QPixmap closed = find(entries, QStringLiteral("valve.closed"))->glyph;
    QVERIFY(near(pixel(closed, corner), ui::theme().error));

    // The padlock, in its badge: the valve's centre is the picture's.
    const QPixmap locked = find(entries, QStringLiteral("valve.locked"))->glyph;
    const QRectF badge = ui::ValveItem::badge_rect().translated(centre(locked));
    QVERIFY(contains_color(locked.copy((badge.toAlignedRect().topLeft() * locked.devicePixelRatio()).x(),
                                       (badge.toAlignedRect().topLeft() * locked.devicePixelRatio()).y(),
                                       qRound(badge.width() * locked.devicePixelRatio()),
                                       qRound(badge.height() * locked.devicePixelRatio())),
                           ui::ValveItem::lock_color()));
    const QPixmap failed = find(entries, QStringLiteral("valve.failed"))->glyph;
    QVERIFY(near(pixel(failed, corner), ui::theme().flash));
  }

  // Changing the canvas's style redraws an open legend.
  void anOpenLegendFollowsTheView() {
    view_->set_valve_style(canvas::ValveStyle::Hmi);
    view_->show_legend(true);
    CanvasLegend* legend = view_->legend();
    QVERIFY(legend->entry(QStringLiteral("valve.open"))->text.contains(QStringLiteral("white glyph")));
    view_->set_valve_style(canvas::ValveStyle::Redgreen);
    const LegendEntry* open = legend->entry(QStringLiteral("valve.open"));
    QVERIFY(open->text.contains(QStringLiteral("green")));
    QVERIFY(near(pixel(open->glyph, centre(open->glyph) + QPointF(-12, 12)), ui::theme().ok));
    view_->set_open_valve_color(canvas::OpenValveColor::Inherit);
    QVERIFY(legend->entry(QStringLiteral("valve.open"))->text.contains(QStringLiteral("section")));
  }

  // Each volume glyph is drawn: no symbol row looks like the plain box.
  void everyVolumeGlyphIsDrawn() {
    const auto entries = ui::legend_entries(view_->legend_inputs());
    const QImage plain = find(entries, QStringLiteral("stage.volume"))->glyph.toImage();
    for (const auto& e : entries) {
      if (!e.key.startsWith(QStringLiteral("stage.")) || e.key == QStringLiteral("stage.volume")) continue;
      QVERIFY2(e.glyph.toImage() != plain, qPrintable(e.key));
    }
  }

 private:
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<CoreBridge> bridge_;
  std::unique_ptr<CanvasView> view_;
};

QTEST_MAIN(TestCanvasLegend)
#include "test_canvas_legend.moc"
