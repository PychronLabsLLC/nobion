#pragma once

#include <cstdint>
#include <vector>

#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::grains {

// Finds grain bodies from the edges that hold still. Frames at least 50 ms
// apart by their timestamps (every frame, where the timestamp does not advance)
// are box-averaged down to at most 800 px wide, and the last segment_window of
// them (at least 8, at most 64) are kept. Per pixel a low quantile of the level over them gives a
// quiet image, and its gradient the edges: a Becke line only adds light, and
// sweeping through focus it is at any one pixel for a few frames, so the quiet
// image is the scene without it. (The gradient of the mean image does not do
// this: a line averaged over a sweep is a broad hump about as strong as a faint
// body edge. Nor does a quantile of the gradient: where a line's flank crosses
// the edge it cancels the step, twice a sweep, and the ring breaks.) A line
// that stays put for most of the window is in the quiet image and can move the
// outline; so can a grain that moved: what was dark stays dark until the window
// has passed, which also holds a vanished grain for most of it.
// Components are recomputed every third frame kept; frames between return the
// last result.
class Segmenter {
 public:
  explicit Segmenter(const GrainOptions&);

  // Components in frame pixels, interiors filled, specks below min_area_px
  // dropped. Empty during warm-up (fewer than 8 frames kept since the start, a
  // reset or a change of frame size) and while the scene is flat.
  std::vector<vision::Component> update(const vision::FrameView&);
  // Too few frames kept to say anything: an empty result means nothing about
  // the scene, and tracks must not age on it. Ask after update().
  [[nodiscard]] bool warming_up() const;
  void reset();

 private:
  std::vector<vision::Component> components_of(const std::vector<float>& stable, double sigma) const;

  GrainOptions options_;
  std::vector<float> buffer_;  // pixel-major: window_ levels per pixel
  std::vector<float> small_;   // the downscaled frame
  std::vector<vision::Component> cached_;
  std::size_t window_ = 0, slot_ = 0, filled_ = 0;
  int width_ = 0, height_ = 0;  // of the input
  int sw_ = 0, sh_ = 0;         // of the downscaled frame
  int factor_ = 1;
  int since_ = 0;
  bool computed_ = false;
  bool admitted_ = false;  // last_admitted_ is good
  TimePoint last_admitted_{};
  std::uint16_t depth_ = 0;
};

}  // namespace pychron::grains
