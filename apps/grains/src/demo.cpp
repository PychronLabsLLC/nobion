#include "demo.hpp"

#include <chrono>
#include <memory>
#include <utility>

namespace pychron::grains {

GrainScene demo_scene() {
  GrainScene s;
  s.width = 320;
  s.height = 240;
  s.grains.push_back({{80, 65}, 25, 1.0, -0.017});
  s.grains.push_back({{240, 65}, 25, 1.0, 0.010});
  s.grains.push_back({{80, 175}, 25, 1.0, 0.0005});
  s.grains.push_back({{240, 175}, 25, 1.0, -0.017});
  return s;
}

std::function<FrameTick()> demo_poll(double speed) {
  struct State {
    GrainSynth synth{demo_scene(), triangle_z(2.0, 1.0), kDemoFps};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::uint64_t next = 1;
  };
  auto state = std::make_shared<State>();
  return [state, speed]() -> FrameTick {
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - state->start;
    if (static_cast<double>(state->next) / kDemoFps > elapsed.count() * speed) return {};
    ++state->next;
    Result<vision::Frame> frame = state->synth.grab();
    if (!frame) return {std::nullopt, frame.error().what};
    return {std::move(*frame), {}};
  };
}

}  // namespace pychron::grains
