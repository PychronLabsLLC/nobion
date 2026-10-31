#pragma once

#include <functional>

#include "pychron/grains/runner.hpp"
#include "pychron/vision/live_feed.hpp"

namespace pychron::grains {

// A poll function over a live feed. A frame is copied out of the feed only
// when its seq is not the one last handed on; otherwise the tick is empty (or
// carries the feed's error). `feed` must outlive the function.
[[nodiscard]] std::function<FrameTick()> live_poll(vision::LiveFeed& feed);

}  // namespace pychron::grains
