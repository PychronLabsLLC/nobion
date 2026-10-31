#pragma once

#include <functional>

#include "pychron/grains/runner.hpp"
#include "pychron/grains/synth.hpp"

namespace pychron::grains {

// The scene `grains --demo` shows: a sanidine, a quartz, a grain close to the
// oil, and a second sanidine at the lower right to click as the reference.
// The reference is not set: the operator does it.
[[nodiscard]] GrainScene demo_scene();
inline constexpr double kDemoFps = 20.0;

// A poll function over a GrainSynth of `scene`, its focus a triangle wave.
// Frames come at the synth's fps times `speed` in real time (steady_clock):
// a poll before the next one is due returns at once with nothing. Tests turn
// `speed` up.
[[nodiscard]] std::function<FrameTick()> demo_poll(double speed = 1.0);

}  // namespace pychron::grains
