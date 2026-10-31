#pragma once

#include <span>
#include <utility>

#include "pychron/core/clock.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::grains {

struct FringeSample {
  double position_px = 0;  // mean signed distance from the outline, outward positive
  double radius_px = 0;    // the same place as a distance from the centroid
  double contrast = 0;     // peak above the profile's baseline, fraction of pixel_depth
  bool valid = false;
};

// Radial brightness profile of a ring about the component's outline, from
// -ring_px to +ring_px, with the edge step taken out by a median baseline (the median runs over
// the whole profile, so a step alone leaves no residual).
// Invalid for a blob cut off by the mask or frame border, one with too few
// boundary points, or a ring that mostly leaves the frame. A valid sample may
// still have no contrast: the caller applies its own minimum.
FringeSample measure_fringe(const vision::FrameView&, const vision::Component&, int ring_px);

// Least-squares slope, px/s, of the samples given. 0 for fewer than two.
double slope_px_s(std::span<const std::pair<TimePoint, double>>);

}  // namespace pychron::grains
