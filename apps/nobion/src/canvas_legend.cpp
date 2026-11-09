#include "canvas_legend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QGraphicsScene>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>

#include "canvas_items.hpp"
#include "theme.hpp"

namespace nobion::ui {

namespace {

constexpr double kGlyphPad = 4;        // around an item wider than the minimum picture
constexpr double kPipeStub = 30;       // a valve's pipe runs this far each side of its centre
constexpr canvas::Size kBox{56, 36};   // a volume's box in the legend

// Draws what is in `scene` at 1:1, centred, on the legend's background.
QPixmap render(QGraphicsScene& scene, const LegendInputs& in) {
  const QRectF drawn = scene.itemsBoundingRect();
  const QSizeF size(std::max<double>(kLegendGlyphWidth, std::ceil(drawn.width() + (2 * kGlyphPad))),
                    std::max<double>(kLegendGlyphHeight, std::ceil(drawn.height() + (2 * kGlyphPad))));
  const QRectF source(drawn.center() - QPointF(size.width() / 2, size.height() / 2), size);
  const qreal dpr = in.device_pixel_ratio > 0 ? in.device_pixel_ratio : 1.0;
  QPixmap pixmap((size * dpr).toSize());
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(in.background.value_or(theme().base));
  {
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    scene.render(&painter, QRectF(QPointF(0, 0), size), source);
  }
  return pixmap;
}

// A valve as the canvas draws one on a horizontal pipe, its caption left out.
template <typename Setup>
QPixmap valve_glyph(const LegendInputs& in, canvas::ValveKind kind, ValveState state, Setup&& setup) {
  QGraphicsScene scene;
  auto* pipe = new ConnectionItem({QPointF(-kPipeStub, 0), QPointF(kPipeStub, 0)}, in.connection_width, {});
  pipe->set_region_color(ConnectionItem::default_color());
  scene.addItem(pipe);
  scene.addItem(pipe->outline());
  auto* valve = new ValveItem("legend", kind);
  valve->set_label(QString());
  valve->set_style(in.valve_style);
  valve->set_pipe_sides(ValveItem::Left | ValveItem::Right);
  valve->set_state(state);
  std::forward<Setup>(setup)(*valve);
  scene.addItem(valve);
  return render(scene, in);
}

QPixmap valve_glyph(const LegendInputs& in, canvas::ValveKind kind, ValveState state) {
  return valve_glyph(in, kind, state, [](ValveItem&) {});
}

template <typename Setup>
QPixmap gauge_glyph(const LegendInputs& in, Setup&& setup) {
  QGraphicsScene scene;
  auto* gauge = new GaugeLabelItem("IG");
  std::forward<Setup>(setup)(*gauge);
  scene.addItem(gauge);
  return render(scene, in);
}

QPixmap box_glyph(const LegendInputs& in, canvas::StageSymbol symbol, const QColor& color) {
  QGraphicsScene scene;
  auto* box = new StageItem("legend", QString(), kBox, color);
  box->set_symbol(symbol);
  box->set_region_color(color);
  scene.addItem(box);
  return render(scene, in);
}

QPixmap pipe_glyph(const LegendInputs& in) {
  QGraphicsScene scene;
  auto* pipe = new ConnectionItem({QPointF(-kPipeStub, 0), QPointF(kPipeStub, 0)}, in.connection_width, {});
  pipe->set_region_color(ConnectionItem::default_color());
  scene.addItem(pipe);
  scene.addItem(pipe->outline());
  return render(scene, in);
}

struct SymbolRow {
  canvas::StageSymbol symbol;
  const char* key;
  const char* text;
};

// The toml spelling (loader's kStageSymbols) and what the glyph stands for.
constexpr std::array<SymbolRow, 8> kSymbols{{
    {canvas::StageSymbol::Spectrometer, "spectrometer", QT_TRANSLATE_NOOP("CanvasLegend", "Spectrometer")},
    {canvas::StageSymbol::Quadrupole, "quadrupole", QT_TRANSLATE_NOOP("CanvasLegend", "Quadrupole")},
    {canvas::StageSymbol::Laser, "laser", QT_TRANSLATE_NOOP("CanvasLegend", "Laser")},
    {canvas::StageSymbol::Turbo, "turbo", QT_TRANSLATE_NOOP("CanvasLegend", "Turbo pump")},
    {canvas::StageSymbol::Getter, "getter", QT_TRANSLATE_NOOP("CanvasLegend", "Getter")},
    {canvas::StageSymbol::IonPump, "ion_pump", QT_TRANSLATE_NOOP("CanvasLegend", "Ion pump")},
    {canvas::StageSymbol::Cryo, "cryo", QT_TRANSLATE_NOOP("CanvasLegend", "Cryo")},
    {canvas::StageSymbol::RoughingPump, "roughing_pump", QT_TRANSLATE_NOOP("CanvasLegend", "Roughing pump")},
}};

QString tr(const char* text) { return CanvasLegend::tr(text); }

}  // namespace

std::vector<LegendEntry> legend_entries(const LegendInputs& in) {
  using canvas::ValveKind;
  const bool redgreen = in.valve_style == canvas::ValveStyle::Redgreen;
  std::vector<LegendEntry> out;
  auto add = [&out](QString section, QString key, QString text, QPixmap glyph) {
    out.push_back({std::move(section), std::move(key), std::move(text), std::move(glyph)});
  };

  const QString valves = tr("Valves");
  add(valves, QStringLiteral("valve.actuated"),
      tr("Valve Nobion drives (dome on its stem), in a box that shows its state. It lies along its pipe, upright "
         "when its pipes leave only above and below."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed));
  add(valves, QStringLiteral("valve.manual"), tr("Manual valve (T bar): moved by hand, its state told to Nobion."),
      valve_glyph(in, ValveKind::Manual, ValveState::Closed));
  add(valves, QStringLiteral("valve.switch"), tr("Switch: pump power, a heater relay, a shutter."),
      valve_glyph(in, ValveKind::Switch, ValveState::Closed));
  // An open box in a section's colour: this line's best source's, or a pump's.
  const bool inherit = !redgreen || in.open_valve_color == canvas::OpenValveColor::Inherit;
  const QColor section = in.sources.empty() ? theme().sources.pump : in.sources.front().color;
  QString open = redgreen ? tr("Open: green box, white glyph.") : tr("Open: white glyph, box in the colour of the section it joins (light grey when that section has no source).");
  if (redgreen && inherit) open = tr("Open: white glyph, box in the colour of the section it joins.");
  add(valves, QStringLiteral("valve.open"), open, valve_glyph(in, ValveKind::Valve, ValveState::Open, [&](ValveItem& v) {
        if (inherit) v.set_inherited_color(section);
      }));
  add(valves, QStringLiteral("valve.closed"),
      redgreen ? tr("Closed: red box, black glyph.") : tr("Closed: grey box, black glyph."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed));
  add(valves, QStringLiteral("valve.unknown"),
      tr("State unknown (white, hatched box, grey glyph): not read yet, or the last command or read-back failed."),
      valve_glyph(in, ValveKind::Valve, ValveState::Unknown));
  add(valves, QStringLiteral("valve.pending"), tr("Command in flight (dashed ring)."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed, [](ValveItem& v) { v.set_pending(true); }));
  add(valves, QStringLiteral("valve.selected"),
      tr("Its faceplate is open (in a rehearsal: the last step changed it)."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed, [](ValveItem& v) { v.set_selected(true); }));
  add(valves, QStringLiteral("valve.locked"),
      tr("Locked in software (padlock): refuses open and close until unlocked from its faceplate."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed, [](ValveItem& v) { v.set_locked(true); }));
  add(valves, QStringLiteral("valve.failed"),
      tr("Flashing yellow: the last command was refused or failed. The reason is in its tooltip and faceplate."),
      valve_glyph(in, ValveKind::Valve, ValveState::Closed, [](ValveItem& v) { v.flash(QString()); }));

  const QString gauges = tr("Gauges");
  add(gauges, QStringLiteral("gauge.reading"),
      tr("A gauge and its latest reading. Rest the pointer on it for its last 10 minutes; click it for its window."),
      gauge_glyph(in, [](GaugeLabelItem& g) { g.set_value(1.2e-8, "torr"); }));
  add(gauges, QStringLiteral("gauge.alarm"), tr("Reading outside its alarm limits."), gauge_glyph(in, [](GaugeLabelItem& g) {
        g.set_value(1.2e-4, "torr");
        g.set_alarm(true);
      }));
  add(gauges, QStringLiteral("gauge.stale"),
      tr("No new reading for three scans (grey, dashed): the value is old, and so is any alarm it showed."),
      gauge_glyph(in, [](GaugeLabelItem& g) {
        g.set_value(1.2e-8, "torr");
        g.set_stale(true, QStringLiteral("45 s"));
      }));
  add(gauges, QStringLiteral("gauge.unwired"), tr("Drawn, but not defined by the line: no reading."),
      gauge_glyph(in, [](GaugeLabelItem& g) { g.set_wired(false); }));

  const QString volumes = tr("Volumes");
  const QColor isolated = theme().neutral_fill;
  add(volumes, QStringLiteral("stage.volume"),
      tr("A volume: a stage, a pipette, a tank. Filled with the colour of its section."),
      box_glyph(in, canvas::StageSymbol::None, isolated));
  for (const SymbolRow& row : kSymbols) {
    add(volumes, QStringLiteral("stage.%1").arg(QLatin1String(row.key)), tr(row.text),
        box_glyph(in, row.symbol, isolated));
  }

  add(tr("Pipes"), QStringLiteral("pipe"), tr("A pipe. It takes the colour of the section it is in."), pipe_glyph(in));

  const QString colours = tr("Section colours");
  for (const auto& source : in.sources) {
    add(colours, QStringLiteral("source.%1").arg(source.key), tr("%1: gas from it, or pumped by it.").arg(source.label),
        box_glyph(in, canvas::StageSymbol::None, source.color));
  }
  add(colours, QStringLiteral("section.none"), tr("No source: isolated."),
      box_glyph(in, canvas::StageSymbol::None, isolated));
  add(colours, QStringLiteral("section.rule"),
      tr("Where sections meet through open valves, the source higher in this list wins."), QPixmap());
  return out;
}

CanvasLegend::CanvasLegend(QWidget* parent) : QWidget(parent, Qt::Tool) {
  setObjectName(QStringLiteral("canvasLegend"));
  setWindowTitle(tr("Canvas legend"));
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  body_ = new QWidget;
  scroll->setWidget(body_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(scroll);
  resize(460, 620);
}

void CanvasLegend::set_entries(std::vector<LegendEntry> entries) {
  entries_ = std::move(entries);
  delete body_->layout();
  for (QWidget* child : body_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) delete child;

  auto* grid = new QGridLayout(body_);
  grid->setContentsMargins(12, 10, 12, 12);
  grid->setHorizontalSpacing(12);
  grid->setVerticalSpacing(4);
  grid->setColumnStretch(1, 1);
  int row = 0;
  QString section;
  for (const LegendEntry& e : entries_) {
    if (e.section != section) {
      section = e.section;
      auto* header = new QLabel(section, body_);
      QFont bold = header->font();
      bold.setBold(true);
      header->setFont(bold);
      if (row > 0) grid->setRowMinimumHeight(row++, 6);
      grid->addWidget(header, row++, 0, 1, 2);
    }
    auto* text = new QLabel(e.text, body_);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setObjectName(QStringLiteral("legend:%1").arg(e.key));
    if (e.glyph.isNull()) {
      grid->addWidget(text, row++, 0, 1, 2);
      continue;
    }
    auto* glyph = new QLabel(body_);
    glyph->setPixmap(e.glyph);
    glyph->setAlignment(Qt::AlignCenter);
    grid->addWidget(glyph, row, 0);
    grid->addWidget(text, row++, 1);
  }
  grid->setRowStretch(row, 1);
}

const LegendEntry* CanvasLegend::entry(const QString& key) const {
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&key](const LegendEntry& e) { return e.key == key; });
  return it == entries_.end() ? nullptr : &*it;
}

void CanvasLegend::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  emit shown_changed(true);
}

void CanvasLegend::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  emit shown_changed(false);
}

void CanvasLegend::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    hide();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace nobion::ui
