#pragma once

#include <cstdint>
#include <vector>

#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::grains {

// Finds grain bodies from the edges that hold still. Frames are box-averaged
// down to at most 800 px wide, and the gradient magnitude of the last
// segment_window frames (at least 8, at most 64) is kept. Per pixel a low
// quantile over them is taken: a body edge is there in nearly every frame, a
// Becke line sweeping through focus is at any one pixel for a few frames
// only. (The gradient of the mean image does not do this: a fringe averaged
// over a sweep is a broad hump about as strong as a faint body edge.)
// Components are recomputed every third frame; frames between return the
// last result.
class Segmenter {
 public:
  explicit Segmenter(const GrainOptions&);

  // Components in frame pixels, interiors filled, specks below min_area_px
  // dropped. Empty during warm-up (fewer than 8 frames since the start, a
  // reset or a change of frame size) and while the scene is flat.
  std::vector<vision::Component> update(const vision::FrameView&);
  void reset();

 private:
  std::vector<vision::Component> components_of(const std::vector<float>& stable, double sigma) const;

  GrainOptions options_;
  std::vector<float> buffer_;  // pixel-major: window_ magnitudes per pixel
  std::vector<float> small_;   // the downscaled frame
  std::vector<vision::Component> cached_;
  std::size_t window_ = 0, slot_ = 0, filled_ = 0;
  int width_ = 0, height_ = 0;  // of the input
  int sw_ = 0, sh_ = 0;         // of the downscaled frame
  int factor_ = 1;
  int since_ = 0;
  bool computed_ = false;
  std::uint16_t depth_ = 0;
};

}  // namespace pychron::grains
