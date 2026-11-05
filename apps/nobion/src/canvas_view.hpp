#pragma once

// CanvasView: builds the scene from the loaded canvas.toml and keeps it in
// step with the CoreBridge snapshot. A left click on a valve selects it and
// opens its faceplate (ValvePopup); with Alt (Option on a Mac) it toggles the
// valve through CoreBridge::actuate. Rejections flash the valve. Clicking a gauge the line
// defines, or resting the pointer on it, is said by a signal; what follows is
// the window's. Stages and pipettes are
// coloured by NetworkGraph region (volumes sharing gas share a colour).
// The drawing never overflows the view: it is scaled, either way, to fit the
// view whatever its size, with no scroll bars and nothing to pan.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QDateTime>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QRectF>
#include <QStringList>
#include <QTimer>

class QToolButton;

#include "canvas_items.hpp"
#include "canvas_legend.hpp"
#include "canvas_state.hpp"
#include "core_bridge.hpp"
#include "valve_popup.hpp"

namespace pychron::ui {

class CanvasView : public QGraphicsView {
  Q_OBJECT

 public:
  // How long the pointer stays on a gauge before gauge_rested: Qt's own
  // tooltip wake-up delay (QStyle::SH_ToolTip_WakeUpDelay), long enough that
  // a pointer passing over a chip opens nothing.
  static constexpr int kGaugeRestMs = 700;
  // Scene units left around the drawn items when the drawing is fitted to the view.
  static constexpr double kContentMargin = 20;

  // Over the bridge: its own adapter, owned by the view.
  explicit CanvasView(CoreBridge& bridge, QWidget* parent = nullptr);
  // Paints from `state` (which must outlive the view). With a null `bridge`
  // the view is read-only: clicks and lock requests do nothing, and nothing
  // here has a path to an actuator.
  CanvasView(ICanvasState& state, CoreBridge* bridge, QWidget* parent = nullptr);
  // Takes the faceplate down first: hiding it ends the selection, which
  // reads the valves, gone once this body has run.
  ~CanvasView() override;
  CanvasView(const CanvasView&) = delete;
  CanvasView& operator=(const CanvasView&) = delete;
  CanvasView(CanvasView&&) = delete;
  CanvasView& operator=(CanvasView&&) = delete;

  [[nodiscard]] bool read_only() const noexcept { return bridge_ == nullptr; }

  void set_gauge_rest_ms(int ms);  // tests: 0
  [[nodiscard]] int gauge_rest_ms() const noexcept { return gauge_rest_timer_.interval(); }

  ValveItem* valve(const std::string& name) const;
  StageItem* stage(const std::string& name) const;  // stages and pipettes
  GaugeLabelItem* gauge(const std::string& name) const;
  int connection_count() const noexcept { return connections_; }
  const std::vector<ConnectionItem*>& pipes() const noexcept { return pipes_; }

  // Locks or unlocks a valve or switch through the bridge. Unlocking asks the
  // confirmation callback first (default: a Yes/No dialog); declining, a
  // manual valve or an unknown name returns false without touching the core.
  // A core error flashes the valve with its reason.
  bool request_lock(const std::string& name, bool locked);
  void set_confirm_unlock(std::function<bool(const QString& name)> confirm) { confirm_unlock_ = std::move(confirm); }

  // Initialised from canvas.toml's open_valve_color; changing it repaints.
  canvas::OpenValveColor open_valve_color() const noexcept { return open_valve_color_; }
  void set_open_valve_color(canvas::OpenValveColor mode);
  // Initialised from canvas.toml's valve_style; changing it repaints.
  canvas::ValveStyle valve_style() const noexcept { return valve_style_; }
  void set_valve_style(canvas::ValveStyle style);

  // Sends `op` for a valve or switch. False, sending nothing, when the view
  // is read-only, the name unknown or a command for it already in flight.
  bool actuate(const std::string& name, systems::SwitchOp op);
  // Alt-click: closes an open valve, opens any other (as one click did
  // before the faceplate).
  bool toggle(const std::string& name);
  // Selects a valve (its accent ring) and shows its faceplate beside it;
  // an unknown name selects nothing. The selection ends when the faceplate
  // closes.
  void select(const std::string& name);
  [[nodiscard]] const std::string& selected() const noexcept { return selected_; }
  // The faceplate; null until a valve was first selected.
  [[nodiscard]] ValvePopup* valve_popup() const noexcept { return popup_; }
  // What the faceplate shows for a valve, as of the state now.
  [[nodiscard]] ValveFaceplate faceplate(const std::string& name) const;

  // The Legend button, top right of the viewport: checked exactly while the
  // legend shows.
  [[nodiscard]] QToolButton* legend_button() const noexcept { return legend_button_; }
  // The legend window; null until it was first shown.
  [[nodiscard]] CanvasLegend* legend() const noexcept { return legend_; }
  void show_legend(bool shown);
  // Marks each wired gauge stale or not as of the line's clock now (a 1 s
  // timer does too): no reading for gauge_stale_after() of the line. A view
  // with no bridge has no readings and marks nothing.
  void check_stale();
  // How old a reading may be; zero without a bridge.
  [[nodiscard]] std::chrono::milliseconds stale_after() const noexcept { return stale_after_; }
  // "45 s" under two minutes, then whole minutes: "3 min".
  static QString age_text(std::chrono::milliseconds age);

  // What the legend is drawn from: this canvas's style and sources.
  [[nodiscard]] LegendInputs legend_inputs() const;

  // The lines of a valve's tooltip under its name, as of `now`: description,
  // then "label\tvalue" rows (style::tip_text): state and how long it has
  // held, owner, last actuation, counts and time open.
  static QStringList valve_details(const CoreBridge::State& state, const std::string& name, const QDateTime& now);

  // Colour of an isolated volume.
  static QColor isolated_color();
  // The canvas file's `[colors] background`, if it names a colour; without
  // one the view keeps the theme's own.
  static std::optional<QColor> background_color(const canvas::Canvas& c);
  // The theme's colour for a source (a region takes its source's). Tanks
  // differ by `ordinal`, their place among the canvas's tanks; the other
  // kinds have one colour each.
  static QColor source_color(canvas::SourceKind kind, int ordinal = 0);
  // What a source's own region is painted: the file's colour for it, else
  // (a pipette) its tank's, else the theme's for its kind.
  static QColor source_display_color(const canvas::Source& source,
                                     const std::map<std::string, canvas::Source, std::less<>>& sources);

 signals:
  void gauge_clicked(const QString& name);
  // The pointer stayed on the gauge for the delay; `global_chip` is its chip
  // on the screen.
  void gauge_rested(const QString& name, const QRect& global_chip);
  // After a rest: the pointer moved off, the view scrolled or was fitted
  // again (a resize), or a button went down or a wheel turned over the view.
  void gauge_left();
  // A valve was selected by a click (its faceplate is open).
  void valve_selected(const QString& name);

 protected:
  void scrollContentsBy(int dx, int dy) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void build(const canvas::Canvas& canvas);
  // Scales the drawing to fill the view (while shown); ends a rest.
  void fit();
  // Widens the scene to the drawn items, plus the margin, when they have
  // grown past it (a gauge's chip widens with its first reading); never
  // narrows it, so the drawing does not jump as readings change.
  void grow_content();
  void on_gauge_hover(const std::string& name, bool entered);
  void on_gauge_rest();
  // Stops a pending rest; after one that was said, says gauge_left.
  void end_gauge_rest();
  void add_path(const std::vector<std::string>& names, double width);
  ConnectionItem* add_pipe(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints);
  bool position(const std::string& name, QPointF& out) const;
  // A connection with an orientation: straight into a box it lines up with,
  // else one corner (vertical first for "v", horizontal first for "h").
  std::vector<QPointF> oriented(QPointF a, const std::string& a_name, QPointF b, const std::string& b_name,
                                bool vertical) const;

  // Records the side a pipe leaves a valve by; `run` starts at the valve's end.
  void note_pipe_side(const std::string& name, const std::vector<QPointF>& run);
  void end_selection();
  void place_legend_button();
  void apply_state();
  void apply_regions();
  void on_pressure(const PressureSample& sample);
  void on_alarm(const Alarm& alarm);
  void on_finished(const QString& name, const Result<void>& result);

  std::unique_ptr<ICanvasState> own_state_;  // set when built over a bridge alone
  ICanvasState& state_;
  CoreBridge* bridge_;  // null: read-only
  std::function<bool(const QString&)> confirm_unlock_;
  canvas::OpenValveColor open_valve_color_ = canvas::OpenValveColor::Green;
  canvas::ValveStyle valve_style_ = canvas::ValveStyle::Hmi;
  std::map<std::string, unsigned> pipe_sides_;  // valves, while the pipes are laid
  std::string selected_;
  ValvePopup* popup_ = nullptr;  // child; made on the first selection
  QToolButton* legend_button_ = nullptr;  // child of the viewport
  CanvasLegend* legend_ = nullptr;        // child; made when first shown
  std::optional<QColor> background_;      // the file's, for the legend
  double connection_width_ = 5;
  QGraphicsScene scene_;
  std::map<std::string, QPointF> positions_;
  std::map<std::string, ValveItem*> valves_;
  std::map<std::string, StageItem*> stages_;
  std::map<std::string, QRectF> boxes_;  // stages and pipettes, scene coordinates
  std::map<std::string, GaugeLabelItem*> gauges_;
  std::map<std::string, canvas::Source, std::less<>> sources_;  // what colours the region it is in
  std::vector<ConnectionItem*> pipes_;
  int connections_ = 0;
  QTimer gauge_rest_timer_;
  QTimer stale_timer_;
  std::chrono::milliseconds stale_after_{0};
  TimePoint watch_from_{};                         // readings expected from here on
  std::map<std::string, TimePoint> last_sample_;  // per gauge, on the line's clock
  std::string gauge_under_;  // the wired gauge the pointer is on, if any
  bool gauge_rested_ = false;
};

}  // namespace pychron::ui
