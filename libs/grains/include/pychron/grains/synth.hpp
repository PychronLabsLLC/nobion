#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/vision/frame.hpp"
#include "pychron/vision/source.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::grains {

struct SynthGrain {
  vision::Vec2 center;
  double radius_px = 40;
  double elongation = 1.0;  // x radius = elongation * radius_px
  double delta_n = 0;       // grain index minus oil index
};

// A Becke scene: grains in oil. A grain body is a step of edge_level below
// the oil at any delta_n. Its Becke line is a gaussian in signed distance
// from the outline (positive outward) of height |delta_n| * fringe_gain. The
// line sits at -sign(delta_n) * fringe_px_per_z * z: raising the focus
// (z > 0) moves it toward the higher index. A delta_n of 0 draws no line.
struct GrainScene {
  int width = 640, height = 480;
  std::uint16_t pixel_depth = 255;
  std::vector<SynthGrain> grains;
  double oil_level = 0.6;      // fraction of pixel_depth
  double edge_level = 0.04;    // step of the grain body: its outline at any delta_n
  double fringe_gain = 10.0;   // fringe height = |delta_n| * fringe_gain, fraction of depth
  double fringe_px_per_z = 8.0;
  double fringe_sigma_px = 2.0;
  double noise = 0.002;        // gaussian sigma, fraction of depth
  std::uint32_t seed = 1;
};

using ZScript = std::function<double(double t_s)>;
using ShiftScript = std::function<vision::Vec2(double t_s)>;

// Triangle wave through 0 at t = 0: up to +amplitude at a quarter period,
// down to -amplitude at three quarters, back to 0 at the period.
ZScript triangle_z(double period_s, double amplitude);

// The scene at focus z, every grain moved by shift. Deterministic for a
// scene (noise is seeded by scene.seed).
vision::Frame render_scene(const GrainScene& scene, double z, vision::Vec2 shift);

// Frame n (from 1) is stamped n / fps after TimePoint{} and rendered at the
// focus and shift the scripts give for that time.
class GrainSynth final : public vision::IFrameSource {
 public:
  GrainSynth(GrainScene scene, ZScript z, double fps, ShiftScript shift = {});

  Result<vision::Frame> grab() override;
  vision::FrameInfo info() const override;

 private:
  GrainScene scene_;
  ZScript z_;
  double fps_;
  ShiftScript shift_;
  std::uint64_t seq_ = 0;
};

}  // namespace pychron::grains
