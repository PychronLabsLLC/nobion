#include "live_poll.hpp"

#include <cstdint>
#include <utility>

namespace pychron::grains {

std::function<FrameTick()> live_poll(vision::LiveFeed& feed) {
  return [&feed, seen = std::uint64_t{0}]() mutable -> FrameTick {
    auto latest = feed.latest_if_newer(seen);
    if (latest.frame) seen = latest.frame->seq;
    return {std::move(latest.frame), std::move(latest.error)};
  };
}

}  // namespace pychron::grains
