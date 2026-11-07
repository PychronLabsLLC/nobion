#include "canvas_view.hpp"
#include "error_report.hpp"
#include "theme.hpp"

#include <QDateTime>
#include <QEvent>
#include <QLocale>
#include <QResizeEvent>
#include <QShowEvent>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

#include <QMessageBox>

#include "nobion/systems/line_pressure_service.hpp"
#include "nobion/systems/network_graph.hpp"

namespace nobion::ui {

namespace {

QColor color_or(const canvas::Canvas& c, const std::string& key, QColor fallback) {
  if (auto it = c.colors.find(key); it != c.colors.end()) {
    QColor parsed(QString::fromStdString(it->second));
    if (parsed.isValid()) {
      return parsed;
    }
  }
  return fallback;
}

// Projection of `p` onto the segment a-b, clamped to it.
QPointF project(QPointF p, QPointF a, QPointF b) {
  const QPointF ab = b - a;
  const double len2 = QPointF::dotProduct(ab, ab);
  if (len2 == 0) {
    return a;
  }
  const double t = std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0);
  return a + t * ab;
}

}  // namespace

QColor CanvasView::isolated_color() { return theme().neutral_fill; }

std::optional<QColor> CanvasView::background_color(const canvas::Canvas& c) {
  const QColor found = color_or(c, "background", QColor());
  return found.isValid() ? std::optional<QColor>(found) : std::nullopt;
}

QColor CanvasView::source_color(canvas::SourceKind kind, int ordinal) {
  const auto& c = theme().sources;
  switch (kind) {
    case canvas::SourceKind::Pump: return c.pump;
    case canvas::SourceKind::Pipette: return c.pipette;
    case canvas::SourceKind::Laser: return c.laser;
    case canvas::SourceKind::Tank: return c.tanks[static_cast<std::size_t>(ordinal < 0 ? 0 : ordinal) % c.tanks.size()];
    case canvas::SourceKind::Spectrometer: return c.spectrometer;
    case canvas::SourceKind::Getter: return c.getter;
    case canvas::SourceKind::None: break;
  }
  return isolated_color();
}

CanvasView::CanvasView(CoreBridge& bridge, QWidget* parent)
    : CanvasView(*new CoreBridgeCanvasState(bridge), &bridge, parent) {
  // The adapter is made before the delegated constructor runs, so it has no
  // owner until here; if that constructor threw it would leak. Nothing in it
  // throws (Qt calls and Result values), and a delegating constructor cannot
  // hold the pointer any earlier.
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): handed to own_state_ at once
  own_state_.reset(&static_cast<CoreBridgeCanvasState&>(state_));
}

CanvasView::CanvasView(ICanvasState& state, CoreBridge* bridge, QWidget* parent)
    : QGraphicsView(parent), state_(state), bridge_(bridge) {
  setScene(&scene_);
  setRenderHint(QPainter::Antialiasing);
  // Always fitted to the view (fit()): nothing to scroll to.
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  gauge_rest_timer_.setSingleShot(true);
  gauge_rest_timer_.setInterval(kGaugeRestMs);
  connect(&gauge_rest_timer_, &QTimer::timeout, this, &CanvasView::on_gauge_rest);
  viewport()->installEventFilter(this);
  legend_button_ = new QToolButton(viewport());
  legend_button_->setObjectName(QStringLiteral("canvasLegendButton"));
  legend_button_->setText(tr("Legend"));
  legend_button_->setToolTip(tr("Show what the canvas symbols mean"));
  legend_button_->setCheckable(true);
  legend_button_->setCursor(Qt::ArrowCursor);
  // A button standing on the drawing: framed at rest, not only under the
  // pointer as a toolbar's are.
  style::make_outlined(legend_button_);
  connect(legend_button_, &QToolButton::toggled, this, [this](bool on) {
    if (on != (legend_ != nullptr && legend_->isVisible())) show_legend(on);
  });
  if (const canvas::Canvas* c = state_.canvas()) {
    valve_style_ = c->canvas.valve_style;
    background_ = background_color(*c);
    connection_width_ = static_cast<double>(c->canvas.connection_width);
    build(*c);
    open_valve_color_ = c->canvas.open_valve_color;
  }

  connect(&state_, &ICanvasState::changed, this, [this] { apply_state(); });
  confirm_unlock_ = [this](const QString& name) {
    return QMessageBox::question(this, tr("Unlock valve"),
                                 tr("Unlock %1? It will accept open and close commands again.").arg(name),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
  };
  if (bridge_) {
    connect(bridge_, &CoreBridge::pressureSample, this, &CanvasView::on_pressure);
    connect(bridge_, &CoreBridge::alarm, this, &CanvasView::on_alarm);
    connect(bridge_, &CoreBridge::actuationStarted, this, [this](const QString& name) {
      if (ValveItem* v = valve(name.toStdString())) {
        v->set_pending(true);
        if (popup_ != nullptr && popup_->isVisible() && selected_ == v->name()) popup_->set_faceplate(faceplate(selected_));
      }
    });
    connect(bridge_, &CoreBridge::actuationFinished, this, &CanvasView::on_finished);
    stale_after_ = systems::gauge_stale_after(bridge_->config().system);
    watch_from_ = bridge_->now();
    stale_timer_.setInterval(1000);
    connect(&stale_timer_, &QTimer::timeout, this, &CanvasView::check_stale);
    stale_timer_.start();
  }
  apply_state();
}

CanvasView::~CanvasView() {
  if (popup_ != nullptr) {
    disconnect(popup_, nullptr, this, nullptr);
    delete popup_;
  }
  // Its hide sets the button, which may already be gone with the viewport.
  if (legend_ != nullptr) {
    disconnect(legend_, nullptr, this, nullptr);
    delete legend_;
  }
  // The scene is destroyed after the members declared below it (the timers,
  // the gauge under the mouse); leaving the view, it resets the scroll range
  // and so calls scrollContentsBy, which reaches them. Leave it now, while
  // they are alive, and with no rest to end: nothing is told of a gauge left
  // by a window going away.
  gauge_rested_ = false;
  setScene(nullptr);
}

QColor CanvasView::source_display_color(const canvas::Source& source,
                                        const std::map<std::string, canvas::Source, std::less<>>& sources) {
  const canvas::Source* shown = &source;
  if (!source.color && !source.tank.empty()) {
    if (auto tank = sources.find(source.tank); tank != sources.end()) shown = &tank->second;
  }
  const QColor own = shown->color ? QColor(QString::fromStdString(*shown->color)) : QColor();
  return own.isValid() ? own : source_color(shown->kind, shown->ordinal);
}

LegendInputs CanvasView::legend_inputs() const {
  LegendInputs in;
  in.valve_style = valve_style_;
  in.open_valve_color = open_valve_color_;
  in.connection_width = connection_width_;
  in.background = background_;
  in.device_pixel_ratio = devicePixelRatioF();
  std::vector<const canvas::Source*> ranked;
  ranked.reserve(sources_.size());
  for (const auto& [name, source] : sources_) ranked.push_back(&source);
  // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order): outranks compares the sources, names last: a total order
  std::sort(ranked.begin(), ranked.end(),
            [](const canvas::Source* a, const canvas::Source* b) { return canvas::outranks(*a, *b); });
  // One row per colour, in the place of its best source: four pumps in the
  // pump colour are one row naming the four.
  std::vector<QStringList> names;
  std::vector<QStringList> kinds;
  for (const canvas::Source* s : ranked) {
    const StageItem* item = stage(s->name);
    const QString name = item != nullptr && !item->label().isEmpty() ? item->label() : QString::fromStdString(s->name);
    QString kind;
    switch (s->kind) {
      case canvas::SourceKind::Pump: kind = tr("pump"); break;
      case canvas::SourceKind::Pipette: kind = tr("pipette"); break;
      case canvas::SourceKind::Laser: kind = tr("laser"); break;
      case canvas::SourceKind::Tank: kind = tr("tank"); break;
      case canvas::SourceKind::Spectrometer: kind = tr("spectrometer"); break;
      case canvas::SourceKind::Getter: kind = tr("getter"); break;
      case canvas::SourceKind::None: break;
    }
    const QColor color = source_display_color(*s, sources_);
    const auto same = std::find_if(in.sources.begin(), in.sources.end(),
                                   [&color](const LegendInputs::Source& row) { return row.color == color; });
    const auto row = static_cast<std::size_t>(same - in.sources.begin());
    if (row == in.sources.size()) {
      in.sources.push_back({QString::fromStdString(s->name), QString(), color});
      names.emplace_back();
      kinds.emplace_back();
    }
    names[row] += name;
    kinds[row] += kind;
  }
  for (std::size_t row = 0; row < in.sources.size(); ++row) {
    QStringList unique = kinds[row];
    unique.removeDuplicates();
    QStringList parts;
    if (unique.size() == 1) {
      parts += names[row].join(QStringLiteral(", "));
      if (!unique.front().isEmpty()) parts.back() += QStringLiteral(" (%1)").arg(unique.front());
    } else {
      for (qsizetype i = 0; i < names[row].size(); ++i) {
        parts += kinds[row][i].isEmpty() ? names[row][i] : QStringLiteral("%1 (%2)").arg(names[row][i], kinds[row][i]);
      }
    }
    in.sources[row].label = parts.join(QStringLiteral(", "));
  }
  return in;
}

void CanvasView::show_legend(bool shown) {
  if (!shown) {
    if (legend_ != nullptr) legend_->hide();
    return;
  }
  if (legend_ == nullptr) {
    legend_ = new CanvasLegend(this);
    // The button follows the window both ways: it also hides and comes back
    // with the window it belongs to.
    connect(legend_, &CanvasLegend::shown_changed, legend_button_, &QToolButton::setChecked);
  }
  legend_->set_entries(legend_entries(legend_inputs()));
  legend_->show();  // checks the button (shown_changed)
  legend_->raise();
}

void CanvasView::place_legend_button() {
  constexpr int kMargin = 8;
  legend_button_->adjustSize();
  legend_button_->move(viewport()->width() - legend_button_->width() - kMargin, kMargin);
  legend_button_->raise();
}

void CanvasView::set_gauge_rest_ms(int ms) { gauge_rest_timer_.setInterval(std::max(ms, 0)); }

void CanvasView::on_gauge_hover(const std::string& name, bool entered) {
  if (!entered) {
    if (name == gauge_under_) end_gauge_rest();
    return;
  }
  end_gauge_rest();
  gauge_under_ = name;
  gauge_rest_timer_.start();
}

void CanvasView::on_gauge_rest() {
  const GaugeLabelItem* item = gauge(gauge_under_);
  if (item == nullptr) return;
  const QRect local = mapFromScene(item->mapRectToScene(item->chip_rect())).boundingRect();
  gauge_rested_ = true;
  emit gauge_rested(QString::fromStdString(gauge_under_), QRect(viewport()->mapToGlobal(local.topLeft()), local.size()));
}

void CanvasView::end_gauge_rest() {
  gauge_rest_timer_.stop();
  gauge_under_.clear();
  if (gauge_rested_) {
    gauge_rested_ = false;
    emit gauge_left();
  }
}

void CanvasView::scrollContentsBy(int dx, int dy) {
  QGraphicsView::scrollContentsBy(dx, dy);
  end_gauge_rest();
}

void CanvasView::resizeEvent(QResizeEvent* event) {
  QGraphicsView::resizeEvent(event);
  place_legend_button();
  fit();
}

void CanvasView::showEvent(QShowEvent* event) {
  QGraphicsView::showEvent(event);
  place_legend_button();
  fit();
}

void CanvasView::fit() {
  // Hidden, the viewport has no size of its own yet: fitted when shown.
  if (!isVisible() || scene_.sceneRect().isEmpty()) return;
  fitInView(scene_.sceneRect(), Qt::KeepAspectRatio);
  // The chip a popup was placed against has moved.
  end_gauge_rest();
}

void CanvasView::grow_content() {
  const QRectF drawn =
      scene_.itemsBoundingRect().adjusted(-kContentMargin, -kContentMargin, kContentMargin, kContentMargin);
  const QRectF now = scene_.sceneRect();
  const QRectF grown = now.isEmpty() ? drawn : now.united(drawn);
  if (grown == now) return;
  // The scene is exactly what is fitted, so the view has nothing beyond it to scroll to.
  scene_.setSceneRect(grown);
  fit();
}

// Sees the viewport's presses and wheels before the scene does; consumes nothing.
bool CanvasView::eventFilter(QObject* watched, QEvent* event) {
  if (watched == viewport()) {
    switch (event->type()) {
      case QEvent::MouseButtonPress:
      case QEvent::MouseButtonDblClick:
      case QEvent::Wheel: end_gauge_rest(); break;
      default: break;
    }
  }
  return QGraphicsView::eventFilter(watched, event);
}

void CanvasView::build(const canvas::Canvas& c) {
  sources_ = canvas::sources(c);
  // The scene is the drawing (grow_content(), at the end), not the file's
  // declared size: what is fitted to the view is what is drawn.
  // Behind the drawing and around it: the view shows the scene's background wherever it is.
  if (const auto background = background_color(c)) scene_.setBackgroundBrush(*background);

  for (const auto& v : c.valves) {
    auto* item = new ValveItem(v.name, v.kind);
    item->setPos(to_qpoint(v.pos));
    item->set_label(QString::fromStdString(v.label()));
    item->set_style(valve_style_);
    item->set_on_actuate([this](const std::string& name) { toggle(name); });
    item->set_on_select([this](const std::string& name) {
      select(name);
      emit valve_selected(QString::fromStdString(name));
    });
    item->set_on_hover([this, item, name = v.name] {
      item->set_details(valve_details(state_.state(), name, state_.wall_now()));
    });
    scene_.addItem(item);
    valves_[v.name] = item;
    positions_[v.name] = item->pos();
  }
  const QColor stage_color = color_or(c, "stage", isolated_color());
  for (const auto& s : c.stages) {
    const std::string& label = s.display_name ? *s.display_name : s.name;
    auto* item = new StageItem(s.name, QString::fromStdString(label), s.size, stage_color);
    item->set_symbol(s.symbol);
    item->setPos(to_qpoint(s.pos));
    scene_.addItem(item);
    stages_[s.name] = item;
    positions_[s.name] = item->pos();
    boxes_[s.name] = QRectF(item->pos().x() - s.size.width / 2, item->pos().y() - s.size.height / 2, s.size.width,
                            s.size.height);
  }
  const QColor pipette_color = color_or(c, "pipette", isolated_color());
  for (const auto& p : c.pipettes) {
    const std::string& label = p.display_name ? *p.display_name : p.vlabel.empty() ? p.name : p.vlabel;
    auto* item = new StageItem(p.name, QString::fromStdString(label), p.size, pipette_color);
    item->setPos(to_qpoint(p.pos));
    scene_.addItem(item);
    stages_[p.name] = item;
    positions_[p.name] = item->pos();
    boxes_[p.name] = QRectF(item->pos().x() - p.size.width / 2, item->pos().y() - p.size.height / 2, p.size.width,
                            p.size.height);
  }
  for (const auto& g : c.gauges) {
    auto* item = new GaugeLabelItem(g.name);
    // A gauge the line defines shows its value, read-only or not.
    item->set_wired(state_.gauge_defined(g.name));
    item->setPos(to_qpoint(g.pos));
    // What follows a click or a rest on a gauge is the window's, and a
    // read-only view has no such window: its gauges only show values.
    if (!read_only()) {
      item->set_on_click([this](const std::string& name) { emit gauge_clicked(QString::fromStdString(name)); });
      item->set_on_hover([this, name = g.name](bool entered) { on_gauge_hover(name, entered); });
    }
    scene_.addItem(item);
    gauges_[g.name] = item;
    positions_[g.name] = item->pos();
  }
  for (const auto& l : c.labels) {
    auto* item = new LabelItem(QString::fromStdString(l.text), QString::fromStdString(l.font));
    item->setPos(to_qpoint(l.pos));
    scene_.addItem(item);
  }

  const auto width = static_cast<double>(c.canvas.connection_width);
  for (const auto& conn : c.connections) {
    QPointF a;
    QPointF b;
    if (!position(conn.start, a) || !position(conn.end, b)) {
      continue;
    }
    a += to_qpoint(conn.start_offset);
    b += to_qpoint(conn.end_offset);
    if (conn.orientation == canvas::Orientation::Auto) {
      add_pipe({a, b}, width, {conn.start, conn.end});
      continue;
    }
    add_pipe(oriented(a, conn.start, b, conn.end, conn.orientation == canvas::Orientation::Vertical), width,
             {conn.start, conn.end});
  }
  for (const auto& e : c.elbows) {
    QPointF a;
    QPointF b;
    if (!position(e.start, a) || !position(e.end, b)) {
      continue;
    }
    a += to_qpoint(e.start_offset);
    b += to_qpoint(e.end_offset);
    if (a.x() == b.x() || a.y() == b.y()) {
      add_pipe({a, b}, width, {e.start, e.end});  // lined up: nothing to turn
      continue;
    }
    const bool left = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::LowerLeft;
    const bool upper = e.corner == canvas::Corner::UpperLeft || e.corner == canvas::Corner::UpperRight;
    QPointF corner(left ? std::min(a.x(), b.x()) : std::max(a.x(), b.x()),
                   upper ? std::min(a.y(), b.y()) : std::max(a.y(), b.y()));
    // The ends sit on two opposite corners of their bounding box and the
    // elbow turns at one of the other two. Asked for a corner an end is on,
    // turn as legacy pychron does: straight up or down from the start.
    if (corner == a || corner == b) {
      corner = QPointF(a.x(), b.y());
    }
    add_pipe({a, corner, b}, width, {e.start, e.end});
  }
  for (const auto& t : c.tees) {
    QPointF l;
    QPointF r;
    QPointF m;
    if (!position(t.left, l) || !position(t.right, r) || !position(t.mid, m)) {
      continue;
    }
    add_pipe({l, r}, width, {t.left, t.right, t.mid});
    add_pipe({m, project(m, l, r)}, width, {t.mid, t.left, t.right});
    --connections_;  // a tee counts once
  }
  for (const auto& x : c.crosses) {
    add_path({x.left, x.right}, width);
    add_path({x.top, x.bottom}, width);
  }
  for (const auto& [name, sides] : pipe_sides_) valves_.at(name)->set_pipe_sides(sides);
  pipe_sides_.clear();
  grow_content();
}

void CanvasView::note_pipe_side(const std::string& name, const std::vector<QPointF>& run) {
  const auto it = valves_.find(name);
  if (it == valves_.end() || run.empty()) return;
  const QPointF at = it->second->pos();
  // A pipe's end is on the valve or beside it (an offset); one that ends
  // elsewhere (a tee's stem meets the bar, not the valve) leaves by no side.
  const QPointF end = run.front() - at;
  if (std::max(std::abs(end.x()), std::abs(end.y())) > ValveItem::kSize) return;
  for (const QPointF& p : run) {
    const QPointF d = p - at;
    if (std::max(std::abs(d.x()), std::abs(d.y())) < 1.0) continue;
    const unsigned side = std::abs(d.x()) >= std::abs(d.y()) ? (d.x() > 0 ? ValveItem::Right : ValveItem::Left)
                                                             : (d.y() > 0 ? ValveItem::Bottom : ValveItem::Top);
    pipe_sides_[name] |= side;
    return;
  }
}

std::vector<QPointF> CanvasView::oriented(QPointF a, const std::string& a_name, QPointF b, const std::string& b_name,
                                          bool vertical) const {
  auto box = [this](const std::string& name) -> const QRectF* {
    auto it = boxes_.find(name);
    return it == boxes_.end() ? nullptr : &it->second;
  };
  if (vertical) {
    if (const QRectF* r = box(b_name); r && a.x() >= r->left() && a.x() <= r->right()) return {a, QPointF(a.x(), b.y())};
    if (const QRectF* r = box(a_name); r && b.x() >= r->left() && b.x() <= r->right()) return {QPointF(b.x(), a.y()), b};
    if (a.x() == b.x()) return {a, b};
    return {a, QPointF(a.x(), b.y()), b};
  }
  if (const QRectF* r = box(b_name); r && a.y() >= r->top() && a.y() <= r->bottom()) return {a, QPointF(b.x(), a.y())};
  if (const QRectF* r = box(a_name); r && b.y() >= r->top() && b.y() <= r->bottom()) return {QPointF(a.x(), b.y()), b};
  if (a.y() == b.y()) return {a, b};
  return {a, QPointF(b.x(), a.y()), b};
}

void CanvasView::add_path(const std::vector<std::string>& names, double width) {
  std::vector<QPointF> points;
  for (const auto& name : names) {
    QPointF p;
    if (!position(name, p)) {
      return;
    }
    points.push_back(p);
  }
  add_pipe(points, width, names);
}

ConnectionItem* CanvasView::add_pipe(const std::vector<QPointF>& points, double width,
                                     std::vector<std::string> endpoints) {
  auto* item = new ConnectionItem(points, width, std::move(endpoints));
  scene_.addItem(item);
  scene_.addItem(item->outline());
  // Break the border of a volume the pipe runs into: the first point belongs
  // to the first endpoint, the last to the second. Only volumes: valves keep
  // a whole border, as in legacy pychron.
  const auto& names = item->endpoints();
  if (points.size() >= 2 && names.size() >= 2) {
    const std::vector<QPointF> reversed(points.rbegin(), points.rend());
    note_pipe_side(names[0], points);
    note_pipe_side(names[1], reversed);
    const std::array<std::pair<const std::vector<QPointF>*, const std::string*>, 2> ends{
        {{&reversed, &names[0]}, {&points, &names[1]}}};
    for (const auto& [run, name] : ends) {
      auto box = boxes_.find(*name);
      if (box == boxes_.end()) continue;
      // Positions are whole pixels, so an end meant for the edge can stop a
      // fraction short of it: look for the crossing a border's width out,
      // then step back in to the edge.
      const double reach = ConnectionItem::kBorderWidth;
      if (auto entry = box_entry(*run, box->second.adjusted(-reach, -reach, reach, reach))) {
        entry->edge += entry->inward * reach;
        scene_.addItem(item->add_gap(*entry, StageItem::border_inset(box->second, *entry, width)));
      }
    }
  }
  pipes_.push_back(item);
  ++connections_;
  return item;
}

bool CanvasView::position(const std::string& name, QPointF& out) const {
  auto it = positions_.find(name);
  if (it == positions_.end()) {
    return false;
  }
  out = it->second;
  return true;
}

ValveItem* CanvasView::valve(const std::string& name) const {
  auto it = valves_.find(name);
  return it == valves_.end() ? nullptr : it->second;
}

StageItem* CanvasView::stage(const std::string& name) const {
  auto it = stages_.find(name);
  return it == stages_.end() ? nullptr : it->second;
}

GaugeLabelItem* CanvasView::gauge(const std::string& name) const {
  auto it = gauges_.find(name);
  return it == gauges_.end() ? nullptr : it->second;
}

bool CanvasView::actuate(const std::string& name, systems::SwitchOp op) {
  if (read_only() || valve(name) == nullptr || state_.pending(name)) {
    return false;
  }
  bridge_->actuate(QString::fromStdString(name), op);
  return true;
}

bool CanvasView::toggle(const std::string& name) {
  const ValveItem* item = valve(name);
  if (item == nullptr) return false;
  return actuate(name, item->state() == ValveState::Open ? systems::SwitchOp::Close : systems::SwitchOp::Open);
}

void CanvasView::select(const std::string& name) {
  ValveItem* item = valve(name);
  if (item == nullptr) return;
  if (ValveItem* before = valve(selected_)) before->set_selected(false);
  selected_ = name;
  item->set_selected(true);
  if (popup_ == nullptr) {
    popup_ = new ValvePopup(this);
    popup_->set_on_operate([this](const std::string& valve_name, systems::SwitchOp op) { actuate(valve_name, op); });
    popup_->set_on_lock([this](const std::string& valve_name, bool locked) {
      request_lock(valve_name, locked);
      // The unlock question is a dialog, and a dialog closes a popup: open
      // the faceplate again on what the answer left.
      select(valve_name);
    });
    connect(popup_, &ValvePopup::closed, this, &CanvasView::end_selection);
  }
  popup_->set_faceplate(faceplate(name));
  const QRect local = mapFromScene(item->mapRectToScene(QRectF(-ValveItem::kSize / 2, -ValveItem::kSize / 2,
                                                                ValveItem::kSize, ValveItem::kSize)))
                          .boundingRect();
  popup_->show_beside(QRect(viewport()->mapToGlobal(local.topLeft()), local.size()));
}

void CanvasView::end_selection() {
  if (ValveItem* item = valve(selected_)) item->set_selected(false);
  selected_.clear();
}

ValveFaceplate CanvasView::faceplate(const std::string& name) const {
  ValveFaceplate f;
  const ValveItem* item = valve(name);
  if (item == nullptr) return f;
  const auto& state = state_.state();
  f.name = name;
  f.title = item->label().isEmpty() ? QString::fromStdString(name) : item->label();
  f.kind = item->kind();
  f.state = item->state();
  f.locked = item->locked();
  f.pending = state_.pending(name);
  f.last_failure = item->last_failure();
  f.operable = !read_only() && item->kind() != canvas::ValveKind::Manual;
  QStringList lines = valve_details(state, name, state_.wall_now());
  const auto info = state.switches.find(name);
  if (info != state.switches.end() && !info->second.description.empty() && !lines.isEmpty()) {
    f.description = lines.takeFirst();
  }
  // The tooltip's rows read as lines here: the state row's value alone, the
  // others "Label: value".
  if (!lines.isEmpty()) f.state_line = lines.takeFirst().section(QLatin1Char('\t'), 1);
  for (QString& line : lines) line.replace(QLatin1Char('\t'), QStringLiteral(": "));
  f.history = lines;
  const auto state_of = [&](const std::string& partner) {
    const auto it = state.valves.find(partner);
    return it == state.valves.end() ? ValveState::Unknown : it->second;
  };
  const systems::InterlockSet set = state_.interlocks(name);
  for (const auto& partner : set.exclusive) {
    f.interlocks.push_back({QString::fromStdString(partner), true, state_of(partner) == ValveState::Closed});
  }
  for (const auto& partner : set.positive) {
    f.interlocks.push_back({QString::fromStdString(partner), false, state_of(partner) == ValveState::Open});
  }
  return f;
}

bool CanvasView::request_lock(const std::string& name, bool locked) {
  ValveItem* item = valve(name);
  if (read_only() || !item || item->kind() == canvas::ValveKind::Manual) {
    return false;
  }
  const QString qname = QString::fromStdString(name);
  if (!locked && !confirm_unlock_(qname)) {
    return false;
  }
  if (auto result = bridge_->set_locked(qname, locked); !result) {
    log_shown("line", locked ? tr("lock %1").arg(qname) : tr("unlock %1").arg(qname), result.error());
    item->flash(QString::fromStdString(result.error().what));
    return false;
  }
  return true;
}

namespace {

// "40 s", "13 min", "2 h 13 min", "17 d 4 h": two units at most.
QString span(std::chrono::seconds d) {
  const qint64 s = std::max<qint64>(0, d.count());
  if (s < 60) return CanvasView::tr("%1 s").arg(s);
  if (s < 3600) return CanvasView::tr("%1 min").arg(s / 60);
  if (s < 86400) return CanvasView::tr("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
  return CanvasView::tr("%1 d %2 h").arg(s / 86400).arg(s % 86400 / 3600);
}

// The time alone today, with the date before it otherwise.
QString moment(systems::WallTime t, const QDateTime& now) {
  const QDateTime at = QDateTime::fromSecsSinceEpoch(t.time_since_epoch().count());
  return QLocale().toString(at, at.date() == now.date() ? QStringLiteral("HH:mm:ss") : QStringLiteral("d MMM yyyy HH:mm"));
}

}  // namespace

// The lines of a valve's tooltip under its name: what it is, the state it is
// in and since when, and its history (kept between runs).
QStringList CanvasView::valve_details(const CoreBridge::State& state, const std::string& name, const QDateTime& now) {
  // Description as a line of its own, the rest label-tab-value rows
  // (style::tip_text lines them up).
  const auto row = [](const QString& label, const QString& value) { return label + QLatin1Char('\t') + value; };
  QStringList lines;
  const auto info = state.switches.find(name);
  if (info != state.switches.end() && !info->second.description.empty())
    lines += QString::fromStdString(info->second.description);

  const auto at = state.valves.find(name);
  const ValveState valve = at == state.valves.end() ? ValveState::Unknown : at->second;
  QString line;
  switch (valve) {
    case ValveState::Open: line = tr("Open"); break;
    case ValveState::Closed: line = tr("Closed"); break;
    default: line = tr("Unknown"); break;
  }
  if (info == state.switches.end()) return lines << row(tr("State"), line);
  const systems::SwitchStats& h = info->second.stats;
  const systems::WallTime wall{std::chrono::seconds(now.toSecsSinceEpoch())};
  if (h.since && valve != ValveState::Unknown)
    line += tr(" since %1 (%2)").arg(moment(*h.since, now), span(wall - *h.since));
  if (info->second.locked) line += tr(", locked");
  lines += row(tr("State"), line);
  if (!info->second.owner.empty()) lines += row(tr("Owner"), QString::fromStdString(info->second.owner));

  if (h.opens == 0 && h.closes == 0 && h.failures == 0 && h.refusals == 0)
    return lines << row(tr("Actuations"), tr("None"));
  if (h.last_actuation) lines += row(tr("Last actuated"), moment(*h.last_actuation, now));
  lines += row(tr("Actuations"), tr("%L1 opened · %L2 closed · %L3 failed · %L4 refused")
                                     .arg(h.opens)
                                     .arg(h.closes)
                                     .arg(h.failures)
                                     .arg(h.refusals));
  // A spell still running counts up to now.
  std::chrono::seconds open = h.open_time;
  if (valve == ValveState::Open && h.since && wall > *h.since) open += wall - *h.since;
  lines += row(tr("Time open"), span(open));
  return lines;
}

void CanvasView::apply_state() {
  const auto& state = state_.state();
  for (auto& [name, item] : valves_) {
    if (auto it = state.valves.find(name); it != state.valves.end()) {
      item->set_state(it->second);
    }
    if (auto it = state.switches.find(name); it != state.switches.end()) {
      item->set_locked(it->second.locked);
    }
    item->set_pending(state_.pending(name));
    item->set_details(valve_details(state, name, state_.wall_now()));
  }
  if (popup_ != nullptr && popup_->isVisible() && !selected_.empty()) popup_->set_faceplate(faceplate(selected_));
  for (auto& [name, item] : gauges_) {
    if (auto it = state.pressures.find(name); it != state.pressures.end()) {
      auto units = state.units.find(name);
      item->set_value(it->second, units == state.units.end() ? std::string{} : units->second);
    }
  }
  apply_regions();
  grow_content();
}

void CanvasView::set_valve_style(canvas::ValveStyle style) {
  valve_style_ = style;
  for (auto& [name, item] : valves_) item->set_style(style);
  apply_regions();  // hmi boxes take their section's colour, redgreen only when asked
  if (legend_ != nullptr) legend_->set_entries(legend_entries(legend_inputs()));
}

void CanvasView::set_open_valve_color(canvas::OpenValveColor mode) {
  open_valve_color_ = mode;
  apply_regions();
  if (legend_ != nullptr) legend_->set_entries(legend_entries(legend_inputs()));
}

void CanvasView::apply_regions() {
  const systems::NetworkGraph* network = state_.network();
  if (!network) {
    for (auto& [name, item] : valves_) {
      item->set_inherited_color(std::nullopt);
    }
    return;
  }
  const auto regions = network->connected_volumes(state_.state().valves);
  // A region takes the colour of the source connected to it with the highest
  // precedence (legacy pychron's rule): a pump over a tank, over a pipette or
  // a laser, over a spectrometer, over a getter. Each tank has a colour of
  // its own, so what is open to a tank shows whose gas it is. It depends on
  // nothing but who is connected, so a region never changes colour because
  // another one did. A region with no source stays neutral; a source alone
  // wears its own colour. Open valves and pipes take the colour of what they
  // join.
  std::map<std::string, QColor> colors;
  for (const auto& region : regions) {
    const canvas::Source* source = canvas::dominant(sources_, region.volumes);
    if (source == nullptr) continue;
    // A pipette holds its tank's gas: it is drawn as the tank is, unless
    // the file gives it a colour of its own.
    const QColor color = source_display_color(*source, sources_);
    for (const auto& volume : region.volumes) colors[volume] = color;
    for (const auto& valve : region.valves) colors[valve] = color;
  }
  // An open valve's box wears the colour of the section it joins: always in
  // the hmi style, in redgreen when the canvas asks (open_valve_color =
  // "inherit"). Every other valve keeps its state colour.
  const bool take = valve_style_ == canvas::ValveStyle::Hmi || open_valve_color_ == canvas::OpenValveColor::Inherit;
  for (auto& [name, item] : valves_) {
    auto it = colors.find(name);
    const bool inherit = take && it != colors.end();
    item->set_inherited_color(inherit ? std::optional<QColor>(it->second) : std::nullopt);
  }
  for (auto& [name, item] : stages_) {
    auto it = colors.find(name);
    item->set_region_color(it == colors.end() ? isolated_color() : it->second);
  }
  for (ConnectionItem* pipe : pipes_) {
    QColor color = ConnectionItem::default_color();
    for (const auto& endpoint : pipe->endpoints()) {
      if (auto it = colors.find(endpoint); it != colors.end()) {
        color = it->second;  // the volume's own fill: through the gap in its border they read as one
        break;
      }
    }
    pipe->set_region_color(color);
  }
}

void CanvasView::on_pressure(const PressureSample& sample) {
  GaugeLabelItem* item = gauge(sample.gauge);
  if (!item) {
    return;
  }
  item->set_value(sample.value, sample.units);
  last_sample_[sample.gauge] = sample.ts;
  if (item->is_stale()) {
    item->set_stale(false);
    item->setToolTip(QString());  // the stale mark's own; a chip has no other
  }
  grow_content();
  if (!item->in_alarm()) {
    return;
  }
  // Clear the alarm colour once a reading is back inside the configured limits.
  if (!bridge_) return;
  for (const auto& g : bridge_->config().gauges) {
    if (g.name == sample.gauge) {
      const bool high = g.alarm_high && sample.value > *g.alarm_high;
      const bool low = g.alarm_low && sample.value < *g.alarm_low;
      item->set_alarm(high || low);
    }
  }
}

void CanvasView::check_stale() {
  if (bridge_ == nullptr || stale_after_.count() <= 0) return;
  const TimePoint now = bridge_->now();
  bool moved = false;
  for (auto& [name, item] : gauges_) {
    if (!item->wired()) continue;
    const auto seen = last_sample_.find(name);
    const TimePoint since = seen == last_sample_.end() ? watch_from_ : seen->second;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - since);
    if (age > stale_after_) {
      const QString before = item->text();
      item->set_stale(true, seen == last_sample_.end() ? tr("no reading") : age_text(age));
      moved = moved || item->text() != before;
      item->setToolTip(item->in_alarm() ? tr("No reading for %1; the last value was in alarm").arg(age_text(age))
                                        : tr("No reading for %1").arg(age_text(age)));
    } else if (item->is_stale()) {
      item->set_stale(false);
      item->setToolTip(QString());
      moved = true;
    }
  }
  if (moved) grow_content();  // a chip widens with its age
}

QString CanvasView::age_text(std::chrono::milliseconds age) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
  if (seconds < 120) return tr("%1 s").arg(seconds);
  return tr("%1 min").arg(seconds / 60);
}

void CanvasView::on_alarm(const Alarm& alarm) {
  if (GaugeLabelItem* item = gauge(alarm.source)) {
    item->set_alarm(true);
  }
}

void CanvasView::on_finished(const QString& name, const Result<void>& result) {
  ValveItem* item = valve(name.toStdString());
  if (!item) {
    return;
  }
  item->set_pending(state_.pending(item->name()));
  if (!result) {
    item->flash(QString::fromStdString(result.error().what));
  }
  if (popup_ != nullptr && popup_->isVisible() && selected_ == item->name()) popup_->set_faceplate(faceplate(selected_));
}

}  // namespace nobion::ui
