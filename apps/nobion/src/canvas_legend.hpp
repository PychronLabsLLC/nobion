#pragma once

// The canvas legend (spec 2026-11-05-canvas-legend): what every symbol on the
// canvas means, each drawn by the canvas's own items so the legend cannot
// drift from what the canvas paints. legend_entries() builds the rows from
// values alone (no bridge, no line state); CanvasLegend is the window that
// shows them, opened by the Legend button on CanvasView.

#include <optional>
#include <vector>

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "nobion/systems/canvas/canvas.hpp"

class QGridLayout;

namespace nobion::ui {

struct LegendEntry {
  QString section;  // "Valves", "Gauges", "Volumes", "Pipes", "Section colours"
  QString key;      // stable id: "valve.open", "stage.turbo", "source.<name>", "section.none"
  QString text;     // what it means
  QPixmap glyph;    // null for a row of text alone
};

struct LegendInputs {
  canvas::ValveStyle valve_style = canvas::ValveStyle::Hmi;
  canvas::OpenValveColor open_valve_color = canvas::OpenValveColor::Green;
  double connection_width = 5;
  std::optional<QColor> background;  // the canvas's; unset: the theme's base
  struct Source {
    QString key;    // the element's name
    QString label;  // "Turbo (pump)"
    QColor color;
  };
  std::vector<Source> sources;  // highest ranked first (canvas::outranks)
  qreal device_pixel_ratio = 1.0;
};

// Glyphs are at least this big, in logical pixels; a wider item (a gauge
// chip) gets a wider picture, never a smaller drawing.
inline constexpr int kLegendGlyphWidth = 72;
inline constexpr int kLegendGlyphHeight = 44;

std::vector<LegendEntry> legend_entries(const LegendInputs& in);

// A tool window: stays open beside the canvas while the operator works it.
class CanvasLegend : public QWidget {
  Q_OBJECT

 public:
  explicit CanvasLegend(QWidget* parent);

  void set_entries(std::vector<LegendEntry> entries);
  [[nodiscard]] const std::vector<LegendEntry>& entries() const noexcept { return entries_; }
  // The entry with `key`; null when there is none.
  [[nodiscard]] const LegendEntry* entry(const QString& key) const;

 signals:
  // Shown or hidden, for any reason: the Legend button, Esc, its close
  // button, or with its window (a tool window hides and comes back with it).
  void shown_changed(bool shown);

 protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  std::vector<LegendEntry> entries_;
  QWidget* body_ = nullptr;
};

}  // namespace nobion::ui
