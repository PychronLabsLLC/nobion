#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "pipeline_scene.hpp"

using namespace pychron::grains;
using namespace pychron::grains::scene;

namespace {

// Piecewise linear through unequal rises, falls and speeds.
double irregular_z(double t) {
  constexpr std::array<std::pair<double, double>, 12> knots{{{0.0, 0.0},
                                                             {0.8, 1.0},
                                                             {1.6, -0.6},
                                                             {2.2, 0.4},
                                                             {3.7, -1.0},
                                                             {4.3, 0.8},
                                                             {5.5, -0.2},
                                                             {6.0, 1.0},
                                                             {7.2, -0.9},
                                                             {7.8, 0.3},
                                                             {9.0, -0.8},
                                                             {10.0, 0.0}}};
  for (std::size_t i = 1; i < knots.size(); ++i) {
    if (t <= knots[i].first) {
      const double f = (t - knots[i - 1].first) / (knots[i].first - knots[i - 1].first);
      return knots[i - 1].second + f * (knots[i].second - knots[i - 1].second);
    }
  }
  return 0.0;
}

void expect_three_labels(const GrainReport& r) {
  EXPECT_EQ(label_at(r, kLow), Label::Candidate);
  EXPECT_EQ(label_at(r, kHigh), Label::Not);
  EXPECT_EQ(label_at(r, kNear), Label::Unsure);
}

}  // namespace

TEST(Pipeline, NoReferenceNoLabel) {
  GrainPipeline p{GrainOptions{}};
  const auto frames = sweep_frames(kSanidine);
  const GrainReport r = feed(p, frames, 0, frames.size());
  EXPECT_EQ(r.reference, ReferenceState::None);
  ASSERT_EQ(r.grains.size(), 4U);
  for (const GrainView& g : r.grains) {
    EXPECT_EQ(g.label, Label::None);
    EXPECT_EQ(g.passes_agree + g.passes_against + g.passes_none, 0);
  }
}

TEST(Pipeline, LabelsThreeGrainsAgainstBelowReference) {
  const GrainReport r = labelled_run(sweep_frames(kSanidine), kRef, Side::Below);
  EXPECT_EQ(r.reference, ReferenceState::Reading);
  expect_three_labels(r);
  const GrainView* reference = grain_at(r, kRef);
  ASSERT_NE(reference, nullptr);
  EXPECT_TRUE(reference->reference);
  EXPECT_EQ(reference->label, Label::None);
  const GrainView* low = grain_at(r, kLow);
  ASSERT_NE(low, nullptr);
  EXPECT_GE(low->score, 0.8);
  EXPECT_LT(low->score, 1.0);
  EXPECT_EQ(low->passes_against, 0);
  EXPECT_EQ(low->last_vote, Vote::Same);
}

TEST(Pipeline, LabelsThreeGrainsAgainstAboveReference) {
  expect_three_labels(labelled_run(sweep_frames(kQuartz), kRef, Side::Above));
}

TEST(Pipeline, ReversedZLeavesEveryLabel) {
  const ZScript up = triangle_z(2, 1);
  const auto frames = sweep_frames(kSanidine, "reversed", [up](double t) { return -up(t); });
  expect_three_labels(labelled_run(frames, kRef, Side::Below));
}

TEST(Pipeline, FlippedDeclaredSideInvertsCandidateAndNot) {
  const GrainReport r = labelled_run(sweep_frames(kSanidine), kRef, Side::Above);
  EXPECT_EQ(label_at(r, kLow), Label::Not);
  EXPECT_EQ(label_at(r, kHigh), Label::Candidate);
  EXPECT_EQ(label_at(r, kNear), Label::Unsure);
}

TEST(Pipeline, IrregularZSameLabels) {
  expect_three_labels(labelled_run(sweep_frames(kSanidine, "irregular", irregular_z), kRef, Side::Below));
}

TEST(Pipeline, ShiftDropsThePass) {
  // Reversals at 0.5, 1.5, ...: the jump at 3.0 is in the middle of a pass.
  constexpr std::size_t kBeforeClose = 69;  // t = 3.45
  constexpr std::size_t kAfterClose = 74;   // t = 3.70
  const ShiftScript jump = [](double t) { return t < 3.0 ? pychron::vision::Vec2{} : pychron::vision::Vec2{20, 0}; };
  const auto shifted = cached("shifted", [&] { return render(four_grains(kSanidine), triangle_z(2, 1), kFps, kFrames, jump); });
  const auto steady = sweep_frames(kSanidine);

  auto passes_over_the_close = [](const std::vector<pychron::vision::Frame>& frames, pychron::vision::Vec2 low) {
    GrainPipeline p{GrainOptions{}};
    feed(p, frames, 0, kClickFrame);
    EXPECT_TRUE(p.set_reference(kRef, Side::Below).has_value());
    const GrainReport before = feed(p, frames, kClickFrame, kBeforeClose);
    const GrainReport after = feed(p, frames, kBeforeClose, kAfterClose);
    return std::pair{passes_at(before, low), passes_at(after, low)};
  };

  const auto [steady_before, steady_after] = passes_over_the_close(steady, kLow);
  EXPECT_EQ(steady_after, steady_before + 1) << "the unshifted run closes this pass";

  // Halfway between where the grain was and where it went: grain_at finds both.
  const auto [shifted_before, shifted_after] = passes_over_the_close(shifted, {kLow.x + 10, kLow.y});
  EXPECT_EQ(shifted_before, steady_before);
  EXPECT_EQ(shifted_after, shifted_before) << "the shifted run gains nothing from it";

  // Grains are followed across the jump and go on voting.
  GrainPipeline p{GrainOptions{}};
  feed(p, shifted, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport end = feed(p, shifted, kClickFrame, shifted.size());
  EXPECT_GT(passes_at(end, {kLow.x + 20, kLow.y}), shifted_after);
}

TEST(Pipeline, TooSlowCameraCountsNoPass) {
  // Slow enough a sweep that passes would form at 5 fps.
  constexpr double kSlowFps = 5.0;
  const auto frames = render(four_grains(kSanidine), triangle_z(8, 1), kSlowFps, 70);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport r = feed(p, frames, kClickFrame, frames.size());
  EXPECT_EQ(r.warning, "camera too slow");
  EXPECT_EQ(total_passes(r), 0);
  EXPECT_EQ(r.reference, ReferenceState::MoveFocus);

  // The same sweep at a rate the camera can meet does count.
  GrainOptions tolerant;
  tolerant.min_fps = 4.0;
  GrainPipeline q(tolerant);
  feed(q, frames, 0, kClickFrame);
  ASSERT_TRUE(q.set_reference(kRef, Side::Below).has_value());
  const GrainReport counted = feed(q, frames, kClickFrame, frames.size());
  EXPECT_TRUE(counted.warning.empty());
  EXPECT_GT(total_passes(counted), 0);
}

TEST(Pipeline, DeterministicForGivenFrames) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline a{GrainOptions{}}, b{GrainOptions{}};
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (i == kClickFrame) {
      ASSERT_TRUE(a.set_reference(kRef, Side::Below).has_value());
      ASSERT_TRUE(b.set_reference(kRef, Side::Below).has_value());
    }
    const GrainReport ra = a.process(frames[i]);
    const GrainReport rb = b.process(frames[i]);
    ASSERT_EQ(ra.seq, rb.seq);
    ASSERT_EQ(ra.timestamp, rb.timestamp);
    ASSERT_EQ(ra.reference, rb.reference);
    ASSERT_EQ(ra.warning, rb.warning);
    ASSERT_EQ(ra.grains.size(), rb.grains.size());
    for (std::size_t g = 0; g < ra.grains.size(); ++g) {
      const GrainView& x = ra.grains[g];
      const GrainView& y = rb.grains[g];
      ASSERT_EQ(x.id, y.id);
      ASSERT_EQ(x.label, y.label);
      // Exact on purpose: same frames, same arithmetic.
      ASSERT_EQ(x.fringe_velocity_px_s, y.fringe_velocity_px_s);
      ASSERT_EQ(x.contrast, y.contrast);
      ASSERT_EQ(x.score, y.score);
      ASSERT_EQ(x.passes_agree, y.passes_agree);
      ASSERT_EQ(x.passes_against, y.passes_against);
      ASSERT_EQ(x.passes_none, y.passes_none);
      ASSERT_EQ(x.last_vote, y.last_vote);
      ASSERT_EQ(x.outline.size(), y.outline.size());
      for (std::size_t k = 0; k < x.outline.size(); ++k) {
        ASSERT_EQ(x.outline[k].x, y.outline[k].x);
        ASSERT_EQ(x.outline[k].y, y.outline[k].y);
      }
    }
  }
}
