#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "pychron/grains/segment.hpp"
#include "pychron/grains/synth.hpp"

using namespace pychron::grains;
using pychron::vision::Component;

namespace {

constexpr double kFps = 10.0;

// Runs the segmenter over `frames` frames of a sweep; returns the components
// of every frame from `from` (1-based) on, last one last.
std::vector<std::vector<Component>> sweep(const GrainScene& scene, int frames, int from, const GrainOptions& options = {}) {
  GrainSynth synth(scene, triangle_z(2, 1), kFps);
  Segmenter seg(options);
  std::vector<std::vector<Component>> out;
  for (int i = 1; i <= frames; ++i) {
    const auto f = synth.grab();
    if (!f) {
      ADD_FAILURE() << "grab failed";
      return out;
    }
    auto found = seg.update(f->view());
    if (i >= from) out.push_back(std::move(found));
  }
  return out;
}

double near_distance(const Component& c, double x, double y) {
  return std::hypot(c.centroid.x - x, c.centroid.y - y);
}

}  // namespace

TEST(Segment, FindsEachGrainOverASweep) {
  GrainScene s;
  s.grains.push_back({{160, 120}, 40, 1.0, -0.017});
  s.grains.push_back({{480, 120}, 40, 1.0, 0.017});
  s.grains.push_back({{320, 360}, 40, 1.0, -0.017});
  const auto all = sweep(s, 20, 20);
  ASSERT_EQ(all.size(), 1U);
  const auto& found = all.back();
  ASSERT_EQ(found.size(), 3U);
  for (const SynthGrain& g : s.grains) {
    const bool matched = std::any_of(found.begin(), found.end(),
                                     [&](const Component& c) { return near_distance(c, g.center.x, g.center.y) < 3.0; });
    EXPECT_TRUE(matched) << "grain at " << g.center.x << "," << g.center.y;
  }
}

TEST(Segment, OutlineStaysOnTheBodyEdgeThroughASweep) {
  GrainScene s;
  s.grains.push_back({{320, 240}, 40, 1.0, -0.017});
  const auto all = sweep(s, 40, 15);
  for (std::size_t i = 0; i < all.size(); ++i) {
    ASSERT_EQ(all[i].size(), 1U) << "frame " << i + 15;
    for (const auto& p : all[i].front().boundary) {
      EXPECT_NEAR(std::hypot(p.x - 320, p.y - 240), 40.0, 4.0) << "frame " << i + 15;
    }
  }
}

TEST(Segment, FindsAFaintGrain) {
  GrainScene s;
  s.edge_level = 0.04;
  s.grains.push_back({{320, 240}, 40, 1.0, 0.0005});
  const auto all = sweep(s, 20, 20);
  ASSERT_EQ(all.back().size(), 1U);
  EXPECT_LT(near_distance(all.back().front(), 320, 240), 3.0);
}

TEST(Segment, DropsSpecksBelowMinArea) {
  GrainScene s;
  s.grains.push_back({{160, 240}, 40, 1.0, -0.017});
  s.grains.push_back({{480, 240}, 6, 1.0, -0.017});  // about 113 px
  const auto all = sweep(s, 20, 20);
  ASSERT_EQ(all.back().size(), 1U);
  EXPECT_LT(near_distance(all.back().front(), 160, 240), 3.0);
}

TEST(Segment, CleanFrameGivesNothing) {
  const GrainScene s;
  const auto all = sweep(s, 20, 1);
  for (const auto& found : all) EXPECT_TRUE(found.empty());
}

TEST(Segment, FlagsAGrainCutByTheBorder) {
  GrainScene s;
  s.grains.push_back({{10, 240}, 40, 1.0, -0.017});
  const auto all = sweep(s, 20, 20);
  ASSERT_EQ(all.back().size(), 1U);
  EXPECT_TRUE(all.back().front().touches_mask_edge);
}
