#pragma once

#include "pychron/vision/frame.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::grains {

// Whole-frame displacement of cur from prev (content at p in prev is at
// p + result in cur), searched to +-search_px.
vision::Vec2 estimate_shift(const vision::FrameView& prev, const vision::FrameView& cur, int search_px);

}  // namespace pychron::grains
