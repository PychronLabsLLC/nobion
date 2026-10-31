#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::grains {

// Finds grain bodies from the edges that hold still. Keeps the gradient
// magnitude of the last segment_window frames and takes, per pixel, a low
// quantile over them: a body edge is there in nearly every frame, a Becke
// line sweeping through focus is at any one pixel for a few frames only.
// (The gradient of the mean image does not do this: a fringe averaged over
// a sweep is a broad hump about as strong as a faint body edge.)
class Segmenter {
 public:
  explicit Segmenter(const GrainOptions&);

  // Components in pixel coordinates, interiors filled, specks below
  // min_area_px dropped. Empty while the scene is flat.
  std::vector<vision::Component> update(const vision::FrameView&);
  void reset();

 private:
  GrainOptions options_;
  std::deque<std::vector<float>> ring_;
  int width_ = 0, height_ = 0;
  std::uint16_t depth_ = 0;
};

}  // namespace pychron::grains
