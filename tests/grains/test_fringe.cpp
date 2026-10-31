#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include "pychron/grains/fringe.hpp"
#include "pychron/grains/synth.hpp"

using namespace pychron::grains;
using pychron::TimePoint;
using pychron::vision::Component;

namespace {

constexpr int kRing = 12;
constexpr double kRadius = 40;

GrainScene one_grain(double delta_n) {
  GrainScene s;
  s.grains.push_back({{320, 240}, kRadius, 1.0, delta_n});
  return s;
}

// The scene's true outline, as the segmenter would give it: pixel centers
// within the body, with a 4-neighbour outside.
Component true_component(const GrainScene& s) {
  const SynthGrain& g = s.grains.front();
  Component c;
  c.area = 3.14159 * g.radius_px * g.radius_px;
  c.centroid = g.center;
  for (int y = 0; y < s.height; ++y) {
    for (int x = 0; x < s.width; ++x) {
      const auto in = [&](int px, int py) {
        return std::hypot((px - g.center.x) / (g.elongation * g.radius_px), (py - g.center.y) / g.radius_px) <= 1.0;
      };
      if (!in(x, y)) continue;
      if (!in(x + 1, y) || !in(x - 1, y) || !in(x, y + 1) || !in(x, y - 1)) {
        c.boundary.push_back({static_cast<double>(x), static_cast<double>(y)});
      }
    }
  }
  return c;
}

FringeSample measure(const GrainScene& s, double z) {
  const auto f = render_scene(s, z, {});
  return measure_fringe(f.view(), true_component(s), kRing);
}

}  // namespace

TEST(Fringe, PositionFollowsZOutwardForLowIndex) {
  const GrainScene s = one_grain(-0.017);
  const FringeSample lo = measure(s, -1.0), hi = measure(s, 1.0);
  ASSERT_TRUE(lo.valid);
  ASSERT_TRUE(hi.valid);
  EXPECT_NEAR(hi.position_px - lo.position_px, 2 * s.fringe_px_per_z, 2.0);
  EXPECT_GT(hi.position_px, lo.position_px);
}

TEST(Fringe, PositionFollowsZInwardForHighIndex) {
  const GrainScene s = one_grain(0.017);
  const FringeSample lo = measure(s, -1.0), hi = measure(s, 1.0);
  ASSERT_TRUE(lo.valid);
  ASSERT_TRUE(hi.valid);
  EXPECT_NEAR(lo.position_px - hi.position_px, 2 * s.fringe_px_per_z, 2.0);
  EXPECT_LT(hi.position_px, lo.position_px);
}

TEST(Fringe, ContrastTracksHeight) {
  const FringeSample m = measure(one_grain(-0.017), 0.5);
  ASSERT_TRUE(m.valid);
  EXPECT_NEAR(m.contrast, 0.17, 0.17 * 0.25);
}

TEST(Fringe, NearOilContrastUnderMinContrast) {
  const FringeSample m = measure(one_grain(0.0005), 0.5);
  EXPECT_LT(m.contrast, 0.01);
}

TEST(Fringe, EdgeStepIsNotAFringe) {
  for (const double z : {-1.0, 0.0, 1.0}) {
    const FringeSample m = measure(one_grain(0.0), z);
    EXPECT_LT(m.contrast, 0.01) << "z " << z;
  }
}

TEST(Fringe, BlobAtBorderIsInvalid) {
  const GrainScene s = one_grain(-0.017);
  const auto f = render_scene(s, 0.5, {});
  Component c = true_component(s);
  c.touches_mask_edge = true;
  EXPECT_FALSE(measure_fringe(f.view(), c, kRing).valid);
}

TEST(Fringe, SlopeOfALine) {
  std::vector<std::pair<TimePoint, double>> v;
  v.reserve(10);
  for (int i = 0; i < 10; ++i) {
    v.emplace_back(TimePoint{} + std::chrono::milliseconds(100 * i), 3.0 + 4.0 * 0.1 * i);
  }
  EXPECT_NEAR(slope_px_s(v), 4.0, 1e-9);
  EXPECT_EQ(slope_px_s(std::span(v).first(1)), 0.0);
  EXPECT_EQ(slope_px_s({}), 0.0);
}
