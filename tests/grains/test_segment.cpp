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

TEST(Segment, FindsAFaintGrainBesideAStrongOne) {
  GrainScene s;
  s.edge_level = 0.04;
  s.grains.push_back({{160, 240}, 40, 1.0, 0.0005});
  s.grains.push_back({{480, 240}, 40, 1.0, -0.017});
  const auto all = sweep(s, 20, 20);
  const auto& found = all.back();
  ASSERT_EQ(found.size(), 2U);
  for (const SynthGrain& g : s.grains) {
    const bool matched = std::any_of(found.begin(), found.end(),
                                     [&](const Component& c) { return near_distance(c, g.center.x, g.center.y) < 3.0; });
    EXPECT_TRUE(matched) << "grain at " << g.center.x;
  }
}

namespace {

// One rendered frame, fed again and again: a scene that holds still.
std::size_t feed_still(Segmenter& seg, const pychron::vision::Frame& frame, int times, std::vector<std::size_t>* counts = nullptr) {
  std::size_t last = 0;
  for (int i = 0; i < times; ++i) {
    last = seg.update(frame.view()).size();
    if (counts != nullptr) counts->push_back(last);
  }
  return last;
}

GrainScene still_scene() {
  GrainScene s;
  s.grains.push_back({{320, 240}, 40, 1.0, 0.0});
  return s;
}

}  // namespace

TEST(Segment, NothingDuringWarmUp) {
  const auto frame = render_scene(still_scene(), 0.0, {});
  Segmenter seg({});
  std::vector<std::size_t> counts;
  feed_still(seg, frame, 8, &counts);
  for (std::size_t i = 0; i < 7; ++i) EXPECT_EQ(counts[i], 0U) << "frame " << i + 1;
  EXPECT_EQ(counts[7], 1U);
}

TEST(Segment, ResetClearsTheRing) {
  const auto frame = render_scene(still_scene(), 0.0, {});
  Segmenter seg({});
  EXPECT_EQ(feed_still(seg, frame, 10), 1U);
  seg.reset();
  std::vector<std::size_t> counts;
  feed_still(seg, frame, 8, &counts);
  for (std::size_t i = 0; i < 7; ++i) EXPECT_EQ(counts[i], 0U) << "frame " << i + 1;
  EXPECT_EQ(counts[7], 1U);
}

TEST(Segment, AChangeOfFrameSizeStartsOver) {
  const auto frame = render_scene(still_scene(), 0.0, {});
  GrainScene small = still_scene();
  small.width = 320;
  small.height = 240;
  small.grains[0].center = {160, 120};
  const auto small_frame = render_scene(small, 0.0, {});
  Segmenter seg({});
  EXPECT_EQ(feed_still(seg, frame, 10), 1U);
  std::vector<std::size_t> counts;
  feed_still(seg, small_frame, 8, &counts);
  for (std::size_t i = 0; i < 7; ++i) EXPECT_EQ(counts[i], 0U) << "frame " << i + 1;
  EXPECT_EQ(counts[7], 1U);
}

TEST(Segment, FindsGrainsDownscaled) {
  GrainScene s;
  s.width = 1920;
  s.height = 1080;
  s.grains.push_back({{480, 300}, 100, 1.0, 0.0});
  s.grains.push_back({{1300, 700}, 80, 1.0, 0.0});
  const auto frame = render_scene(s, 0.0, {});
  Segmenter seg({});
  feed_still(seg, frame, 8);
  const auto found = seg.update(frame.view());
  ASSERT_EQ(found.size(), 2U);
  for (const SynthGrain& g : s.grains) {
    const auto it = std::find_if(found.begin(), found.end(),
                                 [&](const Component& c) { return near_distance(c, g.center.x, g.center.y) < 4.0; });
    ASSERT_NE(it, found.end()) << "grain at " << g.center.x;
    // Back in frame pixels: area near the disc's, boundary near its radius.
    EXPECT_NEAR(it->area, 3.14159 * g.radius_px * g.radius_px, 0.1 * 3.14159 * g.radius_px * g.radius_px);
    for (const auto& p : it->boundary) {
      EXPECT_NEAR(std::hypot(p.x - g.center.x, p.y - g.center.y), g.radius_px, 5.0);
    }
  }
}

TEST(Segment, ALineCrossingTheEdgeNeitherBreaksNorMovesTheOutline) {
  // Height 0.10 over a step of 0.04: the line's flank about cancels the step
  // as it crosses, and at 20 fps it does so within the window.
  constexpr double kRate = 20.0;
  constexpr double kRadius = 25.0;
  GrainScene s;
  s.width = 200;
  s.height = 160;
  s.grains.push_back({{100, 80}, kRadius, 1.0, 0.010});
  GrainSynth synth(s, triangle_z(2, 1), kRate);
  Segmenter seg{GrainOptions{}};
  for (int i = 1; i <= 120; ++i) {
    const auto f = synth.grab();
    ASSERT_TRUE(f.has_value());
    const auto found = seg.update(f->view());
    if (i < 45) continue;  // the window covers a whole sweep
    ASSERT_EQ(found.size(), 1U) << "frame " << i;
    EXPECT_NEAR(std::sqrt(found.front().area / 3.14159), kRadius, 2.0) << "frame " << i;
  }
}

namespace {

// How far from the true radius the outline's mean radius sits, over the frames
// after the ring has filled.
struct OutlineError {
  double worst = 0;
  double share_over_4px = 0;
};

OutlineError sweep_error(double fps, double period_s) {
  constexpr double kRadius = 25.0;
  GrainScene s;
  s.width = 120;
  s.height = 120;
  s.grains.push_back({{60, 60}, kRadius, 1.0, -0.017});
  GrainSynth synth(s, triangle_z(period_s, 1), fps);
  Segmenter seg{GrainOptions{}};
  OutlineError e;
  int counted = 0, over = 0;
  const int frames = static_cast<int>(3.0 * period_s * fps);
  const int settled = static_cast<int>(3.0 * fps);
  for (int i = 1; i <= frames; ++i) {
    const auto f = synth.grab();
    if (!f) {
      ADD_FAILURE() << "grab failed";
      return e;
    }
    const auto found = seg.update(f->view());
    if (i < settled) continue;
    if (found.size() != 1U) {
      ADD_FAILURE() << "frame " << i << ": " << found.size() << " blobs";
      continue;
    }
    double sum = 0;
    for (const pychron::vision::Vec2& p : found.front().boundary) sum += std::hypot(p.x - 60, p.y - 60);
    const double error = std::abs(sum / static_cast<double>(found.front().boundary.size()) + 0.5 - kRadius);
    e.worst = std::max(e.worst, error);
    ++counted;
    over += error > 4.0 ? 1 : 0;
  }
  e.share_over_4px = static_cast<double>(over) / std::max(counted, 1);
  return e;
}

}  // namespace

TEST(Segment, OutlineStaysOnTheBodyEdgeAtAnyFrameRate) {
  for (const double fps : {15.0, 30.0, 60.0}) {
    EXPECT_LT(sweep_error(fps, 2.0).worst, 2.0) << fps << " fps";
  }
}

TEST(Segment, ASlowSweepLeavesTheEdgeForTheSameShareAtAnyFrameRate) {
  // Not a virtue, a limit: at 6 s a sweep the line lingers past what 40 samples
  // span, and the outline stands off the edge near the outer turnaround. The
  // outline is not what the velocity is measured from; what is held here is
  // that this does not get worse with a faster camera.
  const OutlineError slow = sweep_error(20.0, 6.0);
  EXPECT_LT(slow.worst, 9.0);
  EXPECT_LT(slow.share_over_4px, 0.4);
  for (const double fps : {15.0, 30.0, 60.0}) {
    const OutlineError e = sweep_error(fps, 6.0);
    EXPECT_LT(e.worst, slow.worst + 1.0) << fps << " fps";
    EXPECT_LT(e.share_over_4px, slow.share_over_4px + 0.05) << fps << " fps";
  }
}

TEST(Segment, TheRingKeepsFramesByTimeNotByCount) {
  GrainScene s;
  s.width = 120;
  s.height = 120;
  s.grains.push_back({{60, 60}, 25, 1.0, -0.017});
  // 60 fps: every third frame is kept, so eight are kept by frame 22.
  GrainSynth synth(s, triangle_z(2, 1), 60.0);
  Segmenter seg{GrainOptions{}};
  for (int i = 1; i <= 21; ++i) {
    const auto f = synth.grab();
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(seg.update(f->view()).empty()) << "frame " << i;
  }
  const auto last = synth.grab();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(seg.update(last->view()).size(), 1U);
}

TEST(Segment, FramesWhoseTimestampDoesNotAdvanceAreAllKept) {
  GrainScene s;
  s.width = 120;
  s.height = 120;
  s.grains.push_back({{60, 60}, 25, 1.0, 0.0});
  const auto frame = render_scene(s, 0.0, {});  // unstamped
  Segmenter seg{GrainOptions{}};
  for (int i = 1; i < 8; ++i) EXPECT_TRUE(seg.update(frame.view()).empty()) << "frame " << i;
  EXPECT_EQ(seg.update(frame.view()).size(), 1U);
}
