#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "pychron/grains/synth.hpp"

using namespace pychron::grains;
using pychron::vision::Frame;

namespace {

constexpr int kCx = 320, kCy = 240;

GrainScene one_grain(double delta_n) {
  GrainScene s;
  s.noise = 0;
  s.grains.push_back({{kCx, kCy}, 40, 1.0, delta_n});
  return s;
}

// Frame value (fraction of depth) at distance d px outside the outline, on
// the +x cut through the grain center.
double at_offset(const Frame& f, int d) { return static_cast<double>(f.view().at(kCx + 40 + d, kCy)) / f.pixel_depth; }

// Offset from the outline of the highest pixel on the cut, searched over
// [lo, hi].
int peak_offset(const Frame& f, int lo, int hi) {
  int best = lo;
  for (int d = lo; d <= hi; ++d) {
    if (at_offset(f, d) > at_offset(f, best)) best = d;
  }
  return best;
}

}  // namespace

TEST(Synth, FringeMovesOutwardForALowIndexGrainOnRaisedFocus) {
  const GrainScene s = one_grain(-0.017);
  const int at_zero = peak_offset(render_scene(s, 0.0, {}), 1, 20);
  const int at_one = peak_offset(render_scene(s, 1.0, {}), 1, 20);
  EXPECT_GT(at_one, at_zero + 4);
}

TEST(Synth, FringeMovesInwardForAHighIndexGrainOnRaisedFocus) {
  const GrainScene s = one_grain(0.010);
  const int at_zero = peak_offset(render_scene(s, 0.0, {}), -20, -1);
  const int at_one = peak_offset(render_scene(s, 1.0, {}), -20, -1);
  EXPECT_LT(at_one, at_zero - 4);
}

TEST(Synth, FringeHeightScalesWithDeltaN) {
  const GrainScene strong = one_grain(-0.017);
  const GrainScene weak = one_grain(-0.005);
  const double oil = strong.oil_level;
  const Frame fs = render_scene(strong, 0.0, {});
  const Frame fw = render_scene(weak, 0.0, {});
  const double hs = at_offset(fs, peak_offset(fs, 1, 20)) - oil;
  const double hw = at_offset(fw, peak_offset(fw, 1, 20)) - oil;
  ASSERT_GT(hw, 0);
  EXPECT_NEAR(hs / hw, 3.4, 0.34);
}

TEST(Synth, DeterministicForASeed) {
  GrainScene s = one_grain(-0.017);
  s.noise = 0.01;
  const Frame a = render_scene(s, 0.5, {});
  const Frame b = render_scene(s, 0.5, {});
  EXPECT_EQ(a.data, b.data);
  s.seed = 2;
  EXPECT_NE(a.data, render_scene(s, 0.5, {}).data);
}

TEST(Synth, FramesStampedFromFps) {
  GrainSynth synth(one_grain(-0.017), triangle_z(2.0, 1.0), 20.0);
  const auto f1 = synth.grab();
  ASSERT_TRUE(f1.has_value());
  ASSERT_TRUE(synth.grab().has_value());
  const auto f3 = synth.grab();
  ASSERT_TRUE(f3.has_value());
  EXPECT_EQ(f1->seq, 1U);
  EXPECT_EQ(f3->seq, 3U);
  EXPECT_EQ(f3->timestamp - f1->timestamp, std::chrono::milliseconds(100));
}
