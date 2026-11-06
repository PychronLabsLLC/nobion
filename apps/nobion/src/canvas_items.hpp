#pragma once

// QGraphicsItems for the M1 canvas (spec section 10.3). Items are passive:
// they paint whatever state CanvasView pushes into them from the CoreBridge
// snapshot and report clicks through a callback. Positions are element
// centers in canvas.toml coordinates.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QTimer>

#include "nobion/core/events.hpp"
#include "nobion/systems/canvas/canvas.hpp"

#include "theme.hpp"

namespace pychron::ui {

QPointF to_qpoint(const canvas::Point& p);
// A valve's box for its state in a style when it takes no section colour
// (spec 2026-11-04-hmi-valves §4): hmi open the isolated volume's grey and
// closed a darker grey, redgreen open green and closed red; an unknown
// state is the base in both, hatched when painted. An open hmi valve in a
// section with a source wears that section's colour instead.
QColor valve_fill(ValveState state, canvas::ValveStyle style);
// The glyph's fill inside the box, the same in both styles: white open, black
// closed, grey unknown, so the state reads without the box's colour.
QColor valve_tie_fill(ValveState state);

// An ISA-5.1 valve (spec 2026-11-04-hmi-valves): a bow-tie along its pipe
// with the actuator on a stem, a dome for a valve Nobion drives and a T bar
// (the handwheel) for a manual one; a switch is a circle. The state is the
// fill (valve_fill), an unknown state hatched, a command in flight a dashed
// ring, a software lock a padlock badge. The label is a caption beside the
// body on the first side no pipe leaves by. Clicks are on the body only
// (shape()): with Alt (Option on a Mac) a click actuates, any other left
// click selects; a selected or marked valve wears an accent ring. Rejection
// flashes the fill for ~1 s and its reason stays in the tooltip, after the
// name and what CanvasView knows of it (set_details).
class ValveItem : public QGraphicsObject {
  Q_OBJECT

 public:
  static constexpr double kSize = 30.0;
  static constexpr double kBadgeSize = 12.0;

  // The sides of the body pipes leave by, or-ed.
  enum Side : unsigned { Left = 1U, Right = 2U, Top = 4U, Bottom = 8U };

  // The padlock's colour.
  static QColor lock_color() { return theme().lock; }

  ValveItem(std::string name, canvas::ValveKind kind, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  canvas::ValveKind kind() const noexcept { return kind_; }
  ValveState state() const noexcept { return state_; }
  bool locked() const noexcept { return locked_; }
  bool is_pending() const noexcept { return pending_; }
  // Outlined in the accent colour: the rehearsal window's mark for what its
  // last step changed.
  bool marked() const noexcept { return marked_; }
  void set_marked(bool marked);
  // Outlined as marked: the valve whose faceplate is open.
  bool is_selected() const noexcept { return selected_; }
  void set_selected(bool selected);
  bool is_flashing() const noexcept { return flash_ticks_ > 0; }
  QColor fill_color() const;

  canvas::ValveStyle style() const noexcept { return style_; }
  void set_style(canvas::ValveStyle style);

  // Where pipes leave the body (CanvasView sets it as it lays them). From
  // it: the glyph stands upright when pipes leave only above and below, and
  // the caption goes to the first free side of below, above, right, left.
  unsigned pipe_sides() const noexcept { return pipe_sides_; }
  void set_pipe_sides(unsigned sides);
  bool vertical() const noexcept;

  void set_state(ValveState state);
  void set_locked(bool locked);
  // Region colour an open valve shows instead of its style's (CanvasView
  // sets it when canvas.toml says open_valve_color = "inherit"); nullopt =
  // the style's.
  void set_inherited_color(std::optional<QColor> color);
  void set_pending(bool pending);
  // Rejection feedback: blink for ~1 s and keep `what` in the tooltip.
  void flash(const QString& what);
  // The last rejection's reason; empty before the first.
  const QString& last_failure() const noexcept { return last_failure_; }
  // The tooltip's lines under the name: description, state, history.
  void set_details(const QStringList& lines);
  // Called as the pointer comes over the valve, before its tooltip shows:
  // the details hold durations, which are only right when asked for.
  void set_on_hover(std::function<void()> on_hover) { on_hover_ = std::move(on_hover); }

  // A left click with Alt held (Option on a Mac).
  void set_on_actuate(std::function<void(const std::string&)> cb) { on_actuate_ = std::move(cb); }
  // Any other left click.
  void set_on_select(std::function<void(const std::string&)> cb) { on_select_ = std::move(cb); }

  // What the caption says: the name unless set otherwise; may be empty.
  const QString& label() const noexcept { return label_; }
  void set_label(QString label);
  // How the label is drawn with `font` as the scene's: a caption (whenever
  // there is a label) in `font` unchanged.
  struct NameLayout {
    QFont font;
    QString text;
    bool caption = false;
  };
  NameLayout name_layout(const QFont& font) const;
  // The caption's rectangle in item coordinates; empty with no label.
  QRectF caption_rect() const;
  // Where the padlock is drawn, top right of the body, in item coordinates.
  static QRectF badge_rect();

  QRectF boundingRect() const override;
  QPainterPath shape() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;

 private:
  std::string name_;
  QString label_;
  canvas::ValveKind kind_;
  ValveState state_ = ValveState::Unknown;
  canvas::ValveStyle style_ = canvas::ValveStyle::Hmi;
  unsigned pipe_sides_ = 0;
  bool locked_ = false;
  std::optional<QColor> inherited_;
  bool pending_ = false;
  bool marked_ = false;
  bool selected_ = false;
  void update_tip();
  void paint_glyph(QPainter& painter) const;
  void paint_padlock(QPainter& painter) const;
  // The body with its outlines: what a click hits and the item is at least.
  static QRectF body_box();
  QFont item_font() const;

  struct Cached {
    QFont font;
    QString label;
    unsigned sides = 0;
    QRectF caption;
  };
  const Cached& cached(const QFont& font) const;
  mutable std::optional<Cached> cache_;

  std::function<void()> on_hover_;

  QStringList details_;
  QString last_failure_;
  int flash_ticks_ = 0;
  QTimer flash_timer_;
  std::function<void(const std::string&)> on_actuate_;
  std::function<void(const std::string&)> on_select_;
};

// Where a polyline first crosses into `box`, walking from its first point:
// the crossing on the box edge and the unit direction of travel there. A
// pipe need not stop inside the box (a legacy offset can put its end on the
// far edge). Nothing when it starts inside the box or never reaches it.
struct BoxEntry {
  QPointF edge;
  QPointF inward;
};
std::optional<BoxEntry> box_entry(const std::vector<QPointF>& points, const QRectF& box);

// A name set to fit a room: `text` drawn with `font` and `flags` (alignment,
// and Qt::TextWordWrap when it is on two lines) inside the room. Tried in
// order: as it is, wrapped at a space, the font shrunk (to 0.3 of its size),
// and only past that elided with an ellipsis.
struct FittedLabel {
  QFont font;
  QString text;
  int flags = 0;
};
FittedLabel fit_label(const QFont& font, const QString& text, const QSizeF& room);

// A stage or pipette volume; filled with its network region colour.
class StageItem : public QGraphicsItem {
 public:
  static constexpr double kCornerRadius = 8.0;

  StageItem(std::string name, QString label, canvas::Size size, QColor base, QGraphicsItem* parent = nullptr);

  // A glyph drawn with the name, saying what the volume is: above the name
  // when the box is tall enough, else beside it, at its smallest with the
  // name shrunk to the width left, and left out only when the box is too
  // small for even that. The box itself, and so every pipe joining it, is
  // unchanged.
  canvas::StageSymbol symbol() const noexcept { return symbol_; }
  void set_symbol(canvas::StageSymbol symbol);
  // Where paint() puts the glyph for a label this size; empty when it does not fit.
  QRectF symbol_rect(QSizeF label) const;
  // Where the name goes: the box less its padding, or what the glyph leaves.
  QRectF label_room(const QRectF& glyph) const;

  const std::string& name() const noexcept { return name_; }
  const QString& label() const noexcept { return label_; }
  // The name as paint() draws it with `font`: fitted to the box, or to the
  // room beside the glyph when there is one.
  FittedLabel fitted_label(const QFont& font) const;
  // The corner radius of a volume this size: scaled down with the smaller
  // side so thin volumes stay pill-like without swallowing the whole shape.
  static double corner_radius(QSizeF size);
  // How far inside `box`'s straight edge its rounded border runs where a
  // pipe `width` wide crosses at `entry`: 0 along the straight part, up to
  // the corner radius inside a corner.
  static double border_inset(const QRectF& box, const BoxEntry& entry, double width);
  QColor region_color() const noexcept { return region_; }
  void set_region_color(QColor color);

  QRectF boundingRect() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

 private:
  std::string name_;
  struct Layout {
    QRectF glyph;
    QRectF room;
    FittedLabel fitted;
  };
  struct Cached {
    QFont font;
    QSizeF room;
    FittedLabel fitted;
  };
  // The one place the glyph, the room and the fitted name are worked out:
  // paint() and fitted_label() both take it. The fit is kept until the font
  // or the room changes.
  Layout layout_for(const QFont& font) const;

  QString label_;
  QRectF rect_;
  QColor region_;
  mutable std::optional<Cached> cache_;
  canvas::StageSymbol symbol_ = canvas::StageSymbol::None;
};

// Plumbing drawn as a polyline through element centers. Remembers the names
// of the elements it joins so the view can paint it in the colour of the
// network region it belongs to.
//
// A pipe is bordered like every other component, as in legacy pychron. The
// item itself is the fill; the border is outline(), the same path a border
// wider on each side, which the view stacks beneath every pipe's fill so
// pipes that meet (a tee, an elbow's corner) merge with no line between them.
// Where a pipe enters a volume, a gap (add_gap) paints the pipe's fill over
// the volume's border: the border is broken there.
class ConnectionItem : public QGraphicsPathItem {
 public:
  static constexpr double kBorderWidth = 1.0;
  // Stacking: borders, then fills, then (above the volumes at z 1) gaps.
  static constexpr double kOutlineZ = -1.0;
  static constexpr double kFillZ = 0.0;
  static constexpr double kGapZ = 1.5;

  ConnectionItem(const std::vector<QPointF>& points, double width, std::vector<std::string> endpoints,
                 QGraphicsItem* parent = nullptr);

  static QColor default_color();

  const std::vector<std::string>& endpoints() const noexcept { return endpoints_; }
  QColor region_color() const { return pen().color(); }
  void set_region_color(QColor color);

  // Scene-level items the view adds beside this one (the scene owns them).
  QGraphicsPathItem* outline() const noexcept { return outline_; }
  const std::vector<QGraphicsPathItem*>& gaps() const noexcept { return gaps_; }
  // `depth`: how far in from the edge the volume's border lies at its
  // deepest across the pipe's width (more than the border itself only
  // inside a rounded corner; see StageItem::border_inset).
  QGraphicsPathItem* add_gap(const BoxEntry& entry, double depth = 0);

 private:
  std::vector<std::string> endpoints_;
  QGraphicsPathItem* outline_ = nullptr;
  std::vector<QGraphicsPathItem*> gaps_;
};

class LabelItem : public QGraphicsSimpleTextItem {
 public:
  LabelItem(const QString& text, const QString& font_spec, QGraphicsItem* parent = nullptr);
  // "Arial 12" -> family Arial, 12 pt; a bare family or size is accepted.
  static QFont parse_font(const QString& spec);
};

// A gauge: a chip holding a dial, the name and the value + units; red while in alarm.
class GaugeLabelItem : public QGraphicsSimpleTextItem {
 public:
  explicit GaugeLabelItem(std::string name, QGraphicsItem* parent = nullptr);

  const std::string& name() const noexcept { return name_; }
  bool in_alarm() const noexcept { return alarm_; }
  void set_value(double value, const std::string& units);
  void set_alarm(bool alarm);

  // No new reading for too long (spec 2026-11-05-hmi-banner §7): the value
  // and dial go grey, the chip's outline dashed, and `age` ("45 s") follows
  // the value. Grey wins over the alarm red: an old value's alarm is not
  // current either. The next reading clears it.
  bool is_stale() const noexcept { return stale_; }
  void set_stale(bool stale, const QString& age = {});

  // False for a gauge the line does not define: drawn for illustration, its
  // name with no reading.
  bool wired() const noexcept { return wired_; }
  void set_wired(bool wired);

  // A small dial left of the reading marks it as a gauge; both sit on a chip.
  QRectF dial_rect() const;
  QRectF chip_rect() const;
  QRectF boundingRect() const override;
  // The chip, dial and padding included: what takes a click or a hover.
  QPainterPath shape() const override;
  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

  // A wired gauge only: a left click on the chip, and the pointer coming
  // onto it (true) or leaving it (false). An unwired one lets both through
  // to what is under it.
  void set_on_click(std::function<void(const std::string&)> cb) { on_click_ = std::move(cb); }
  void set_on_hover(std::function<void(bool entered)> cb) { on_hover_ = std::move(cb); }

 protected:
  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
  void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
  void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

 private:
  void refresh();
  void apply_wired();

  std::function<void(const std::string&)> on_click_;
  std::function<void(bool)> on_hover_;
  std::string name_;
  QString value_ = QStringLiteral("--");
  bool alarm_ = false;
  bool stale_ = false;
  QString age_;
  bool wired_ = true;
};

}  // namespace pychron::ui
