#include <gtest/gtest.h>

#include "pychron/grains/shift.hpp"
#include "pychron/grains/synth.hpp"

using namespace pychron::grains;

namespace {

GrainScene busy_scene() {
  GrainScene s;
  s.grains.push_back({{200, 180}, 40, 1.0, -0.017});
  s.grains.push_back({{420, 300}, 50, 1.0, 0.017});
  s.grains.push_back({{300, 340}, 30, 1.0, -0.017});
  return s;
}

}  // namespace

TEST(Shift, FindsAnImposedShift) {
  const GrainScene s = busy_scene();
  const auto a = render_scene(s, 0.0, {});
  const auto b = render_scene(s, 0.0, {5, -3});
  const auto d = estimate_shift(a.view(), b.view(), 12);
  EXPECT_NEAR(d.x, 5, 1.0);
  EXPECT_NEAR(d.y, -3, 1.0);
}

TEST(Shift, ZeroForFocusChangeAlone) {
  const GrainScene s = busy_scene();
  const auto a = render_scene(s, 0.0, {});
  const auto b = render_scene(s, 0.5, {});
  const auto d = estimate_shift(a.view(), b.view(), 12);
  EXPECT_NEAR(d.x, 0, 1.0);
  EXPECT_NEAR(d.y, 0, 1.0);
}
