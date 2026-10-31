#include "grain_overlay.hpp"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include "overlay_tokens.hpp"
#include "pychron/grains/score.hpp"

namespace pychron::grains {

namespace {

QImage to_image(const vision::Frame& frame) {
  if (frame.width <= 0 || frame.height <= 0 ||
      frame.data.size() < static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) {
    return {};
  }
  QImage image(frame.width, frame.height, QImage::Format_Grayscale8);
  const double depth = frame.pixel_depth > 0 ? frame.pixel_depth : 255;
  for (int y = 0; y < frame.height; ++y) {
    uchar* row = image.scanLine(y);
    const std::uint16_t* from = frame.data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width);
    for (int x = 0; x < frame.width; ++x) row[x] = static_cast<uchar>(std::clamp(from[x] * 255.0 / depth, 0.0, 255.0));
  }
  return image;
}

QPolygonF polygon(const GrainView& grain) {
  QPolygonF poly;
  poly.reserve(static_cast<qsizetype>(grain.outline.size()));
  for (const vision::Vec2& p : grain.outline) poly << QPointF(p.x, p.y);
  return poly;
}

QPointF centroid(const GrainView& grain) {
  if (grain.outline.empty()) return {};
  double x = 0, y = 0;
  for (const vision::Vec2& p : grain.outline) {
    x += p.x;
    y += p.y;
  }
  const auto n = static_cast<double>(grain.outline.size());
  return {x / n, y / n};
}

QColor label_colour(Label label) {
  switch (label) {
    case Label::Candidate: return tokens::kCandidate;
    case Label::Not: return tokens::kNot;
    case Label::Unsure: return tokens::kUnsure;
    case Label::None: break;
  }
  return tokens::kNoReference;
}

}  // namespace

GrainOverlay::GrainOverlay(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("grain_overlay"));
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMinimumSize(320, 240);
}

void GrainOverlay::show_frame(const vision::Frame& frame, const GrainReport& report) {
  image_ = to_image(frame);
  report_ = report;
  failed_ = false;
  failed_why_.clear();
  const auto reference = std::ranges::find_if(report_.grains, [](const GrainView& g) { return g.reference; });
  if (reference != report_.grains.end()) reference_id_ = reference->id;
  if (report_.reference == ReferenceState::None) reference_id_.reset();
  update();
}

void GrainOverlay::set_failed(const QString& why) {
  failed_ = true;
  failed_why_ = why;
  update();
}

void GrainOverlay::set_picked(std::uint64_t id, bool picked) {
  if (picked) {
    picked_.insert(id);
  } else {
    picked_.erase(id);
  }
  update();
}

void GrainOverlay::clear_picked() {
  picked_.clear();
  update();
}

bool GrainOverlay::is_picked(std::uint64_t id) const { return picked_.contains(id); }

void GrainOverlay::set_side(Side side) {
  side_ = side;
  update();
}

void GrainOverlay::set_notice(const QString& notice) {
  notice_ = notice;
  update();
}

void GrainOverlay::set_selected(std::optional<std::uint64_t> id) {
  selected_ = id;
  update();
}

QRectF GrainOverlay::picture() const {
  if (image_.isNull()) return {};
  const double scale = std::min(width() / static_cast<double>(image_.width()), height() / static_cast<double>(image_.height()));
  const double w = image_.width() * scale, h = image_.height() * scale;
  return {(width() - w) / 2, (height() - h) / 2, w, h};
}

QPointF GrainOverlay::to_widget(QPointF frame_px) const {
  const QRectF box = picture();
  if (box.isEmpty()) return {};
  const double scale = box.width() / image_.width();
  // a pixel's centre is at its integer coordinate
  return {box.left() + (frame_px.x() + 0.5) * scale, box.top() + (frame_px.y() + 0.5) * scale};
}

QPointF GrainOverlay::to_frame(QPointF widget) const {
  const QRectF box = picture();
  if (box.isEmpty()) return {};
  const double scale = box.width() / image_.width();
  return {(widget.x() - box.left()) / scale - 0.5, (widget.y() - box.top()) / scale - 0.5};
}

QString GrainOverlay::banner_text() const {
  QStringList parts;
  if (report_.reference == ReferenceState::None) {
    parts << tr("no reference: pick a known grain with the reference tool");
  } else {
    const QString id = reference_id_ ? QString::number(*reference_id_) : QStringLiteral("?");
    const QString side = side_ == Side::Below ? tr("below oil") : tr("above oil");
    QString state;
    switch (report_.reference) {
      case ReferenceState::Lost: state = tr("lost, labels held"); break;
      case ReferenceState::MoveFocus: state = tr("move focus"); break;
      case ReferenceState::Reading: state = tr("reading"); break;
      case ReferenceState::Disagree: state = tr("references disagree, labels stopped"); break;
      case ReferenceState::None: break;
    }
    parts << tr("reference %1, %2: %3").arg(id, side, state);
  }
  if (failed_) parts << tr("camera: %1").arg(failed_why_);
  if (!report_.warning.empty()) parts << QString::fromStdString(report_.warning);
  if (!notice_.isEmpty()) parts << notice_;
  return parts.join(QStringLiteral("  |  "));
}

bool GrainOverlay::labels_shown() const {
  return report_.reference != ReferenceState::None && report_.reference != ReferenceState::Disagree;
}

bool GrainOverlay::greyed(const GrainView& grain) const {
  return failed_ || report_.reference == ReferenceState::Lost || picked_.contains(grain.id);
}

QColor GrainOverlay::outline_colour(const GrainView& grain) const {
  if (!labels_shown() || grain.label == Label::None) return tokens::kNoReference;
  if (greyed(grain)) return tokens::kGreyed;
  return label_colour(grain.label);
}

std::vector<DrawnLabel> GrainOverlay::drawn_labels() const {
  std::vector<DrawnLabel> out;
  if (!labels_shown()) return out;
  for (const GrainView& grain : report_.grains) {
    QString text;
    if (grain.reference) {
      text = tr("reference");
    } else if (grain.label != Label::None) {
      text = QString::fromUtf8(label_text(grain.label).data(), static_cast<qsizetype>(label_text(grain.label).size()));
      text += QStringLiteral("  %1").arg(grain.score, 0, 'f', 2);
    } else {
      continue;
    }
    if (picked_.contains(grain.id)) text += tr("  picked");
    out.push_back({grain.id, text, greyed(grain) ? tokens::kGreyed : label_colour(grain.label), centroid(grain)});
  }
  return out;
}

QStringList GrainOverlay::labels() const {
  QStringList out;
  for (const DrawnLabel& label : drawn_labels()) out << label.text;
  return out;
}

std::optional<std::uint64_t> GrainOverlay::grain_at(QPointF frame_px) const {
  for (const GrainView& grain : report_.grains) {
    if (grain.outline.size() < 3) continue;
    if (polygon(grain).containsPoint(frame_px, Qt::OddEvenFill)) return grain.id;
  }
  return std::nullopt;
}

void GrainOverlay::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || image_.isNull() || !picture().contains(event->position())) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  emit frame_clicked(to_frame(event->position()));
}

void GrainOverlay::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), tokens::kChrome);
  if (image_.isNull()) {
    p.setPen(tokens::kOnChrome);
    p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, failed_ ? failed_why_ : tr("no picture yet"));
  } else {
    const QRectF box = picture();
    const double scale = box.width() / image_.width();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(box, image_);
    p.setRenderHint(QPainter::Antialiasing, true);

    for (const GrainView& grain : report_.grains) {
      if (grain.outline.size() < 3) continue;
      QPolygonF poly;
      for (const vision::Vec2& v : grain.outline) poly << to_widget({v.x, v.y});
      const bool dim = failed_ || picked_.contains(grain.id);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(dim ? tokens::kGreyed : outline_colour(grain), grain.reference ? 3 : 2));
      p.drawPolygon(poly);
      if (grain.reference) {
        p.setPen(QPen(tokens::kReferenceRing, 1, Qt::DashLine));
        const QRectF bounds = poly.boundingRect().adjusted(-4, -4, 4, 4);
        p.drawEllipse(bounds);
      }
      if (selected_ && *selected_ == grain.id) {
        p.setPen(QPen(tokens::kSelected, 1, Qt::DotLine));
        p.drawRect(poly.boundingRect().adjusted(-6, -6, 6, 6));
      }
      if (labels_shown() && grain.label != Label::None && !grain.reference) {
        // a tick per pass: agree, against, none
        const QRectF bounds = poly.boundingRect();
        const double tick = std::max(3.0, 2.0 * scale);
        double x = bounds.left();
        const double y = bounds.bottom() + 4;
        p.setPen(Qt::NoPen);
        // agree is toward the label, so it takes the label's colour
        const bool held = dim || report_.reference == ReferenceState::Lost;
        const auto ticks = [&](int count, const QColor& colour, bool filled) {
          const QColor shade = held ? tokens::kGreyed : colour;
          p.setBrush(filled ? QBrush(shade) : QBrush(Qt::NoBrush));
          p.setPen(filled ? QPen(Qt::NoPen) : QPen(shade, 1));
          for (int i = 0; i < count; ++i) {
            p.drawRect(QRectF(x, y, tick, tick));
            x += tick * 1.5;
          }
        };
        ticks(grain.passes_agree, label_colour(grain.label), true);
        ticks(grain.passes_against, tokens::kNoReference, true);
        ticks(grain.passes_none, tokens::kNoReference, false);
      }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    for (const DrawnLabel& label : drawn_labels()) {
      const QPointF at = to_widget(label.at);
      const QRectF text_box = p.fontMetrics().boundingRect(label.text);
      QRectF back(at.x() - text_box.width() / 2 - 3, at.y() - text_box.height() / 2 - 1, text_box.width() + 6, text_box.height() + 2);
      p.fillRect(back, tokens::kBannerBackground);
      p.setPen(label.colour);
      p.drawText(back, Qt::AlignCenter, label.text);
    }
    if (failed_) {
      QColor veil = tokens::kChrome;
      veil.setAlpha(tokens::kVeilAlpha);
      p.fillRect(box, veil);
      p.setPen(tokens::kOnChrome);
      p.drawText(box.adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap, failed_why_);
    }
  }
  const QString banner = banner_text();
  const QRect strip(0, 0, width(), p.fontMetrics().height() + 10);
  p.fillRect(strip, tokens::kBannerBackground);
  p.setPen(tokens::kBannerText);
  p.drawText(strip.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, banner);
}

}  // namespace pychron::grains
