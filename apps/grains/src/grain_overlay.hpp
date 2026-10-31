#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::grains {

// One label as it is drawn, in frame pixels.
struct DrawnLabel {
  std::uint64_t id = 0;
  QString text;
  QColor colour;
  QPointF at;
};

// The video, the outlines, the labels and the banner. Draws the way
// apps/pychron-ui CameraView does: the picture fitted into the widget, pixel
// centres at integer coordinates.
class GrainOverlay : public QWidget {
  Q_OBJECT
 public:
  explicit GrainOverlay(QWidget* parent = nullptr);

  void show_frame(const vision::Frame& frame, const GrainReport& report);
  // The last picture stays, greyed, under the reason; labels are held.
  void set_failed(const QString& why);
  void set_picked(std::uint64_t id, bool picked);
  void clear_picked();
  [[nodiscard]] bool is_picked(std::uint64_t id) const;
  // The side declared for the reference, as the banner shows it.
  void set_side(Side side);
  // A line the banner carries after the pipeline's own warning.
  void set_notice(const QString& notice);
  void set_selected(std::optional<std::uint64_t> id);

  [[nodiscard]] QPointF to_frame(QPointF widget) const;
  [[nodiscard]] QPointF to_widget(QPointF frame_px) const;
  [[nodiscard]] QString banner_text() const;
  // What is drawn as a label, none while no reference is set or the two
  // references disagree.
  [[nodiscard]] std::vector<DrawnLabel> drawn_labels() const;
  [[nodiscard]] QStringList labels() const;
  [[nodiscard]] bool has_picture() const { return !image_.isNull(); }
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const GrainReport& report() const { return report_; }
  // The grain whose outline holds the point, if any.
  [[nodiscard]] std::optional<std::uint64_t> grain_at(QPointF frame_px) const;
  [[nodiscard]] QSize sizeHint() const override { return {640, 480}; }

 signals:
  void frame_clicked(QPointF frame_px);

 protected:
  void paintEvent(QPaintEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;

 private:
  [[nodiscard]] QRectF picture() const;
  [[nodiscard]] bool labels_shown() const;
  [[nodiscard]] bool greyed(const GrainView& grain) const;
  [[nodiscard]] QColor outline_colour(const GrainView& grain) const;

  QImage image_;
  GrainReport report_;
  Side side_ = Side::Below;
  std::optional<std::uint64_t> reference_id_;  // last seen, kept while lost
  std::optional<std::uint64_t> selected_;
  std::set<std::uint64_t> picked_;
  QString failed_why_;
  QString notice_;
  bool failed_ = false;
};

}  // namespace pychron::grains
