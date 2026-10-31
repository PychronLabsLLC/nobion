#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pipeline_scene.hpp"

using namespace pychron::grains;
using namespace pychron::grains::scene;
using pychron::ErrorKind;
using pychron::vision::Frame;
using pychron::vision::Vec2;

namespace {

// A reference near the left border, the others well in. Three jumps left walk
// it out of the frame; each is small enough for the others to be followed.
constexpr Vec2 kWalkRef{40, 120};
constexpr Vec2 kWalkLow{170, 65};
constexpr Vec2 kWalkHigh{270, 65};
constexpr Vec2 kWalkNear{170, 175};
constexpr double kWalkStep = 22.0;
constexpr double kFirstJump = 7.0;
constexpr std::size_t kBeforeJump = 139;
constexpr std::size_t kWalkFrames = 280;

constexpr double kStopAt = 6.25;  // a sweep held still from here on

Vec2 walked(Vec2 at) { return {at.x - 3 * kWalkStep, at.y}; }

std::vector<Frame> walking_frames() {
  return cached("walking", [] {
    GrainScene s;
    s.width = 320;
    s.height = 240;
    s.grains.push_back({kWalkLow, 25, 1.0, kSanidine});
    s.grains.push_back({kWalkHigh, 25, 1.0, kQuartz});
    s.grains.push_back({kWalkNear, 25, 1.0, kNearOil});
    s.grains.push_back({kWalkRef, 25, 1.0, kSanidine});
    const ShiftScript walk = [](double t) {
      const int jumps = (t >= kFirstJump ? 1 : 0) + (t >= kFirstJump + 0.5 ? 1 : 0) + (t >= kFirstJump + 1.0 ? 1 : 0);
      return Vec2{-kWalkStep * jumps, 0};
    };
    return render(s, triangle_z(2, 1), kFps, static_cast<int>(kWalkFrames), walk);
  });
}

// Runs the walk until the reference's track has ended.
GrainReport walk_until_lost(GrainPipeline& p) {
  const auto frames = walking_frames();
  feed(p, frames, 0, kClickFrame);
  EXPECT_TRUE(p.set_reference(kWalkRef, Side::Below).has_value());
  return feed(p, frames, kClickFrame, 200);
}

void expect_same_counts(const GrainView& a, const GrainView& b) {
  EXPECT_EQ(a.passes_agree, b.passes_agree);
  EXPECT_EQ(a.passes_against, b.passes_against);
  EXPECT_EQ(a.passes_none, b.passes_none);
  EXPECT_EQ(a.score, b.score);
  EXPECT_EQ(a.label, b.label);
}

// A sweep in which `gone` is out of the scene for frames [from, to).
std::vector<Frame> with_gap(std::size_t gone, std::size_t from, std::size_t to) {
  auto frames = sweep_frames(kSanidine);
  GrainScene without = four_grains(kSanidine);
  without.grains.erase(without.grains.begin() + static_cast<std::ptrdiff_t>(gone));
  const auto empty = cached("without" + std::to_string(gone), [&] { return render(without, triangle_z(2, 1), kFps, kFrames); });
  for (std::size_t i = from; i < to; ++i) frames[i] = empty[i];
  return frames;
}

constexpr std::size_t kRefIndex = 3, kLowIndex = 0;

// reset_at and jump_at are turnarounds. Each restarts the ring with the line at
// its turn, so for the first 2 s the quiet image has seen little of the sweep.
void expect_no_wrong_vote(double period_s, double reset_at, double jump_at) {
  const ShiftScript jump = [jump_at](double t) { return t < jump_at ? Vec2{} : Vec2{20, 0}; };
  const auto frames = render(four_grains(kSanidine), triangle_z(period_s, 1), kFps, 260, jump);
  const auto reset_frame = static_cast<std::size_t>(reset_at * kFps);

  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  feed(p, frames, kClickFrame, reset_frame);
  p.reset();
  const GrainReport r = feed(p, frames, reset_frame, frames.size());

  EXPECT_NE(r.reference, ReferenceState::Lost);
  const GrainView* a = grain_at(r, {kLow.x + 20, kLow.y});
  const GrainView* b = grain_at(r, {kHigh.x + 20, kHigh.y});
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_GT(a->passes_agree, 0);
  EXPECT_GT(b->passes_agree, 0);
  EXPECT_EQ(a->passes_against, 0);
  EXPECT_EQ(b->passes_against, 0);
  EXPECT_EQ(a->last_vote, Vote::Same);
  EXPECT_EQ(b->last_vote, Vote::Opposite);
}

}  // namespace

TEST(PipelineReference, RefusedOffAnyGrain) {
  GrainPipeline p{GrainOptions{}};
  const auto frames = sweep_frames(kSanidine);
  feed(p, frames, 0, kClickFrame);
  const auto clicked = p.set_reference({160, 120}, Side::Below);
  ASSERT_FALSE(clicked.has_value());
  EXPECT_EQ(clicked.error().kind, ErrorKind::Config);
  EXPECT_NE(clicked.error().what.find("no grain"), std::string::npos) << clicked.error().what;
  EXPECT_FALSE(p.reference_mark().has_value());
}

TEST(PipelineReference, RefusedWhenNoFringeIsMeasured) {
  // Whole in the frame, but the ring round it is not: tracked, nothing to read.
  GrainScene s;
  s.width = 90;
  s.height = 90;
  s.grains.push_back({{45, 45}, 25, 1.0, kSanidine});
  const auto frames = render(s, triangle_z(2, 1), kFps, kClickFrame);
  GrainOptions wide;
  wide.ring_px = 30;
  GrainPipeline p(wide);
  ASSERT_EQ(feed(p, frames, 0, frames.size()).grains.size(), 1U);
  const auto clicked = p.set_reference({45, 45}, Side::Below);
  ASSERT_FALSE(clicked.has_value());
  EXPECT_EQ(clicked.error().kind, ErrorKind::Config);
  EXPECT_NE(clicked.error().what.find("no fringe measured"), std::string::npos) << clicked.error().what;
}

TEST(PipelineReference, RefusedWhenContrastTooLow) {
  GrainPipeline p{GrainOptions{}};
  const auto frames = sweep_frames(kSanidine);
  feed(p, frames, 0, kClickFrame);
  const auto clicked = p.set_reference(kNear, Side::Below);
  ASSERT_FALSE(clicked.has_value());
  EXPECT_EQ(clicked.error().kind, ErrorKind::Config);
  EXPECT_NE(clicked.error().what.find("contrast"), std::string::npos) << clicked.error().what;
  EXPECT_FALSE(p.reference_mark().has_value());
}

TEST(PipelineReference, LostHoldsLabelsAndStopsVotes) {
  const auto frames = walking_frames();
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kWalkRef, Side::Below).has_value());
  const GrainReport before = feed(p, frames, kClickFrame, kBeforeJump);
  const GrainView* low_before = grain_at(before, kWalkLow);
  const GrainView* high_before = grain_at(before, kWalkHigh);
  ASSERT_NE(low_before, nullptr);
  ASSERT_NE(high_before, nullptr);
  EXPECT_EQ(low_before->label, Label::Candidate);
  EXPECT_GT(low_before->passes_agree, 0);

  const GrainReport end = feed(p, frames, kBeforeJump, frames.size());
  EXPECT_EQ(end.reference, ReferenceState::Lost);
  EXPECT_EQ(grain_at(end, walked(kWalkRef)), nullptr);
  const GrainView* low_end = grain_at(end, walked(kWalkLow));
  const GrainView* high_end = grain_at(end, walked(kWalkHigh));
  ASSERT_NE(low_end, nullptr);
  ASSERT_NE(high_end, nullptr);
  EXPECT_EQ(low_end->id, low_before->id);
  expect_same_counts(*low_before, *low_end);
  expect_same_counts(*high_before, *high_end);
}

TEST(PipelineReference, ReClickRecoversFromLost) {
  GrainPipeline p{GrainOptions{}};
  const GrainReport lost = walk_until_lost(p);
  ASSERT_EQ(lost.reference, ReferenceState::Lost);

  ASSERT_TRUE(p.set_reference(walked(kWalkLow), Side::Below).has_value());
  const auto frames = walking_frames();
  const GrainReport end = feed(p, frames, 200, frames.size());
  EXPECT_NE(end.reference, ReferenceState::Lost);
  EXPECT_NE(end.reference, ReferenceState::None);
  const GrainView* low_end = grain_at(end, walked(kWalkLow));
  const GrainView* high_end = grain_at(end, walked(kWalkHigh));
  ASSERT_NE(low_end, nullptr);
  ASSERT_NE(high_end, nullptr);
  EXPECT_TRUE(low_end->reference);
  EXPECT_GT(high_end->passes_agree + high_end->passes_against, 0);
}

TEST(PipelineReference, SameGrainClickedAgainKeepsVotesAndAChangedSideInverts) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport before = feed(p, frames, kClickFrame, 160);
  ASSERT_EQ(label_at(before, kLow), Label::Candidate);
  ASSERT_EQ(label_at(before, kHigh), Label::Not);

  ASSERT_TRUE(p.set_reference(kRef, Side::Above).has_value());
  const GrainReport after = feed(p, frames, 160, 161);
  EXPECT_EQ(passes_at(after, kLow), passes_at(before, kLow));
  EXPECT_EQ(passes_at(after, kHigh), passes_at(before, kHigh));
  EXPECT_EQ(label_at(after, kLow), Label::Not);
  EXPECT_EQ(label_at(after, kHigh), Label::Candidate);
}

TEST(PipelineReference, ClickingAnotherGrainClearsTheVotes) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport before = feed(p, frames, kClickFrame, 100);
  ASSERT_GT(passes_at(before, kHigh), 0);

  ASSERT_TRUE(p.set_reference(kLow, Side::Below).has_value());
  const GrainReport after = feed(p, frames, 100, 101);
  EXPECT_EQ(total_passes(after), 0);
}

TEST(PipelineReference, NoWrongVoteThroughAResetAndAJumpInAFastSweep) { expect_no_wrong_vote(2.0, 3.5, 6.5); }
TEST(PipelineReference, NoWrongVoteThroughAResetAndAJumpInASlowSweep) { expect_no_wrong_vote(6.0, 4.5, 7.5); }

TEST(PipelineReference, SecondReferenceOppositeSideAgrees) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(p.set_second_reference(kHigh, Side::Above).has_value());
  const GrainReport r = feed(p, frames, kClickFrame, frames.size());
  EXPECT_EQ(r.reference, ReferenceState::Reading);
  EXPECT_EQ(label_at(r, kLow), Label::Candidate);
  const GrainView* second = grain_at(r, kHigh);
  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(second->reference);
  EXPECT_EQ(second->label, Label::None);
  EXPECT_GE(second->passes_agree, GrainOptions{}.disagree_passes);
  EXPECT_EQ(second->passes_against, 0);
}

TEST(PipelineReference, SecondReferenceMovingSameWayDisagrees) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(p.set_second_reference(kLow, Side::Above).has_value());

  bool seen = false;
  for (std::size_t i = kClickFrame; i < frames.size(); ++i) {
    const GrainReport r = p.process(frames[i]);
    if (r.reference != ReferenceState::Disagree) {
      ASSERT_FALSE(seen) << "Disagree came and went at frame " << i;
      continue;
    }
    if (!seen) {
      const GrainView* second = grain_at(r, kLow);
      ASSERT_NE(second, nullptr);
      EXPECT_EQ(second->passes_agree, GrainOptions{}.disagree_passes);
    }
    seen = true;
    for (const GrainView& g : r.grains) EXPECT_EQ(g.label, Label::None);
  }
  EXPECT_TRUE(seen);
}

TEST(PipelineReference, DisagreeOutlivesTheSecondReferencesTrack) {
  // The second reference leaves the view: labels must not come back on a sign
  // it contradicted.
  const auto frames = sweep_frames(kSanidine);
  GrainScene without = four_grains(kSanidine);
  without.grains.erase(without.grains.begin());  // kLow
  const auto after = cached("no-low", [&] { return render(without, triangle_z(2, 1), kFps, 60); });
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(p.set_second_reference(kLow, Side::Above).has_value());
  ASSERT_EQ(feed(p, frames, kClickFrame, 100).reference, ReferenceState::Disagree);

  // The rest of the run, stamped on from where it left off.
  std::vector<Frame> tail = after;
  const auto interval = frames[1].timestamp - frames[0].timestamp;
  for (std::size_t i = 0; i < tail.size(); ++i) {
    tail[i].timestamp = frames[99].timestamp + interval * static_cast<long>(i + 1);
  }
  const GrainReport r = feed(p, tail, 0, tail.size());
  EXPECT_EQ(grain_at(r, kLow), nullptr);
  EXPECT_EQ(r.reference, ReferenceState::Disagree);
  for (const GrainView& g : r.grains) EXPECT_EQ(g.label, Label::None);
}

TEST(PipelineReference, GrainCutByEdgeStaysUnsure) {
  constexpr Vec2 kCut{4, 120};
  GrainScene s = four_grains(kSanidine);
  s.grains.push_back({kCut, 25, 1.0, kSanidine});
  const auto frames = render(s, triangle_z(2, 1), kFps, kFrames);
  const GrainReport r = labelled_run(frames, kRef, Side::Below);
  EXPECT_EQ(label_at(r, kLow), Label::Candidate);
  auto cut = std::find_if(r.grains.begin(), r.grains.end(), [](const GrainView& g) { return g.at_edge; });
  ASSERT_NE(cut, r.grains.end());
  EXPECT_LT(middle(*cut).x, 30.0);
  EXPECT_EQ(cut->label, Label::Unsure);
  EXPECT_EQ(cut->passes_agree + cut->passes_against + cut->passes_none, 0);
}

TEST(PipelineReference, TrackLostPastGapResetsVotes) {
  // The quiet image holds a vanished grain for 34 frames; then comes the gap.
  constexpr std::size_t kGoes = 100, kBack = 160;
  const auto whole = sweep_frames(kSanidine);
  // kLow leaves the view for kBack - kGoes frames; the tail of the run is the
  // whole scene again.
  const auto gone = cached("gone", [] {
    GrainScene s = four_grains(kSanidine);
    s.grains.erase(s.grains.begin());
    return render(s, triangle_z(2, 1), kFps, static_cast<int>(kBack));
  });

  GrainPipeline p{GrainOptions{}};
  feed(p, whole, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport before = feed(p, whole, kClickFrame, kGoes);
  const GrainView* low_before = grain_at(before, kLow);
  ASSERT_NE(low_before, nullptr);
  ASSERT_GE(low_before->passes_agree, 3);
  const std::uint64_t old_id = low_before->id;

  const GrainReport absent = feed(p, gone, kGoes, kBack);
  EXPECT_EQ(grain_at(absent, kLow), nullptr);

  // First report that has it again: a new track with nothing behind it.
  std::optional<GrainView> back;
  for (std::size_t i = kBack; i < whole.size() && !back; ++i) {
    const GrainReport r = p.process(whole[i]);
    if (const GrainView* g = grain_at(r, kLow)) back = *g;
  }
  ASSERT_TRUE(back.has_value());
  EXPECT_NE(back->id, old_id);
  EXPECT_EQ(back->passes_agree + back->passes_against + back->passes_none, 0);
  EXPECT_EQ(back->label, Label::Unsure);
}

TEST(PipelineReference, StationaryFocusStaysMoveFocus) {
  const ZScript up = triangle_z(2, 1);
  const auto frames = sweep_frames(kSanidine, "stopped", [up](double t) { return up(std::min(t, kStopAt)); });
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport moving = feed(p, frames, kClickFrame, 140);
  const GrainView* low_moving = grain_at(moving, kLow);
  ASSERT_NE(low_moving, nullptr);
  EXPECT_GT(low_moving->passes_agree, 0);

  GrainReport stopped;
  for (std::size_t i = 140; i < frames.size(); ++i) {
    stopped = p.process(frames[i]);
    // A parked line enters the quiet image over the window, and the outline
    // settling can read as a frame or two of motion: too short to be a pass.
    if (i >= 170) {
      EXPECT_EQ(stopped.reference, ReferenceState::MoveFocus) << "frame " << i;
    }
  }
  const GrainView* low_stopped = grain_at(stopped, kLow);
  ASSERT_NE(low_stopped, nullptr);
  expect_same_counts(*low_moving, *low_stopped);
}

TEST(PipelineReference, VeryFastRackFormsNoPass) {
  // Reverses every 3 frames at 20 fps.
  const auto frames = sweep_frames(kSanidine, "fast", triangle_z(0.3, 1));
  const GrainReport r = labelled_run(frames, kRef, Side::Below);
  EXPECT_EQ(total_passes(r), 0);
  EXPECT_EQ(label_at(r, kLow), Label::Unsure);
}

TEST(PipelineReference, NewFrameSizeResetsAndDropsTheReferences) {
  const auto frames = sweep_frames(kSanidine);
  GrainScene small = four_grains(kSanidine);
  small.width = 300;
  small.height = 230;
  const auto other = render(small, triangle_z(2, 1), kFps, 30);

  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport big = feed(p, frames, kClickFrame, 30);
  ASSERT_EQ(big.grains.size(), 4U);

  const GrainReport first = p.process(other[0]);
  EXPECT_TRUE(first.grains.empty());
  EXPECT_FALSE(p.reference_mark().has_value());
  EXPECT_EQ(first.reference, ReferenceState::None);
  EXPECT_NE(first.warning.find("camera changed"), std::string::npos) << first.warning;

  const GrainReport later = feed(p, other, 1, kClickFrame);
  ASSERT_EQ(later.grains.size(), 4U);
  EXPECT_EQ(later.reference, ReferenceState::None);
  EXPECT_EQ(later.warning, first.warning) << "held until a reference is set";
  for (const GrainView& g : later.grains) {
    EXPECT_LE(g.id, 4U) << "ids restart";
    EXPECT_EQ(g.label, Label::None);
    EXPECT_EQ(g.passes_agree + g.passes_against + g.passes_none, 0);
  }

  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  EXPECT_TRUE(feed(p, other, kClickFrame, other.size()).warning.empty());
}

TEST(PipelineReference, ACameraChangeWithNoReferenceSaysNothing) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, 10);
  EXPECT_TRUE(p.process(pychron::vision::Frame::make(100, 80, 255, 90)).warning.empty());
}

TEST(PipelineReference, NewPixelDepthDropsTheReferences) {
  const auto frames = sweep_frames(kSanidine);
  GrainScene deep = four_grains(kSanidine);
  deep.pixel_depth = 4095;
  const auto other = render(deep, triangle_z(2, 1), kFps, 2);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  feed(p, frames, kClickFrame, 20);
  const GrainReport r = p.process(other[0]);
  EXPECT_FALSE(p.reference_mark().has_value());
  EXPECT_EQ(r.reference, ReferenceState::None);
  EXPECT_NE(r.warning.find("camera changed"), std::string::npos) << r.warning;
}

TEST(PipelineReference, BlankFrameResetsAndDropsTheReferences) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  feed(p, frames, kClickFrame, 30);
  const GrainReport blank = p.process(Frame{});
  EXPECT_TRUE(blank.grains.empty());
  EXPECT_EQ(blank.reference, ReferenceState::None);
  EXPECT_FALSE(p.reference_mark().has_value());
  EXPECT_NE(blank.warning.find("camera changed"), std::string::npos) << blank.warning;
  const GrainReport first = p.process(frames[30]);
  EXPECT_TRUE(first.grains.empty());
  EXPECT_EQ(first.warning, blank.warning);
}

TEST(PipelineReference, GenerationCountsResetsOnly) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  const GrainReport start = feed(p, frames, 0, 3);
  EXPECT_EQ(feed(p, frames, 3, 5).generation, start.generation);
  p.reset();
  const GrainReport after_reset = feed(p, frames, 5, 6);
  EXPECT_EQ(after_reset.generation, start.generation + 1);
  const GrainReport resized = p.process(pychron::vision::Frame::make(100, 80, 255, 90));
  EXPECT_EQ(resized.generation, start.generation + 2);
  EXPECT_EQ(p.process(Frame{}).generation, start.generation + 3);
}

TEST(PipelineReference, AJumpOnAFastCameraKeepsTracksAndVotes) {
  // At 60 fps the ring takes 22 frames to refill, more than track_gap_frames.
  constexpr double kFast = 60.0;
  constexpr double kJumpAt = 3.0;
  constexpr Vec2 kOne{50, 60}, kTwo{150, 60}, kThree{235, 60};
  GrainScene s;
  s.width = 300;
  s.height = 120;
  s.grains.push_back({kOne, 25, 1.0, kSanidine});
  s.grains.push_back({kTwo, 25, 1.0, kQuartz});
  s.grains.push_back({kThree, 25, 1.0, kSanidine});
  const ShiftScript jump = [](double t) { return t < kJumpAt ? Vec2{} : Vec2{20, 0}; };
  const auto frames = render(s, triangle_z(2, 1), kFast, 300, jump);

  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, 36);
  ASSERT_TRUE(p.set_reference(kThree, Side::Below).has_value());
  const auto jump_frame = static_cast<std::size_t>(kJumpAt * kFast);
  const GrainReport before = feed(p, frames, 36, jump_frame - 2);
  const GrainView* one_before = grain_at(before, kOne);
  ASSERT_NE(one_before, nullptr);
  ASSERT_GT(one_before->passes_agree, 0);

  // Blind while the ring refills: the reference reads Lost, nothing ages.
  const GrainReport blind = feed(p, frames, jump_frame - 2, jump_frame + 10);
  EXPECT_EQ(blind.reference, ReferenceState::Lost);
  EXPECT_EQ(blind.grains.size(), before.grains.size());

  const GrainReport end = feed(p, frames, jump_frame + 10, frames.size());
  EXPECT_NE(end.reference, ReferenceState::Lost);
  const GrainView* one_end = grain_at(end, {kOne.x + 20, kOne.y});
  ASSERT_NE(one_end, nullptr);
  EXPECT_EQ(one_end->id, one_before->id);
  EXPECT_GT(one_end->passes_agree, one_before->passes_agree) << "votes held, and more cast after";
}

TEST(PipelineReference, AGapReadsLostAndRecoversByItself) {
  // The quiet image holds a vanished grain about 34 frames, so 38 frames out
  // gives a gap shorter than track_gap_frames.
  const auto frames = with_gap(kRefIndex, 100, 138);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport before = feed(p, frames, kClickFrame, 100);
  const GrainView* reference = grain_at(before, kRef);
  ASSERT_NE(reference, nullptr);
  const std::uint64_t id = reference->id;

  bool lost = false;
  std::optional<GrainReport> first_lost, last_lost;
  for (std::size_t i = 100; i < 160; ++i) {
    const GrainReport r = p.process(frames[i]);
    if (r.reference == ReferenceState::Lost) {
      lost = true;
      if (!first_lost) first_lost = r;
      last_lost = r;
    }
  }
  ASSERT_TRUE(lost) << "the gap never happened";
  const GrainView* a = grain_at(*first_lost, kLow);
  const GrainView* b = grain_at(*last_lost, kLow);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  expect_same_counts(*a, *b);

  const GrainReport end = feed(p, frames, 160, frames.size());
  EXPECT_NE(end.reference, ReferenceState::Lost);
  const GrainView* back = grain_at(end, kRef);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->id, id) << "the same track, tracked again";
  EXPECT_TRUE(back->reference);
}

TEST(PipelineReference, AnEndedTrackRecoversOnlyByAReClick) {
  const auto frames = with_gap(kRefIndex, 80, 150);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  feed(p, frames, kClickFrame, 150);

  const GrainReport back = feed(p, frames, 150, 185);
  const GrainView* blob = grain_at(back, kRef);
  ASSERT_NE(blob, nullptr) << "the grain is back in view";
  EXPECT_FALSE(blob->reference);
  EXPECT_EQ(back.reference, ReferenceState::Lost);

  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  EXPECT_NE(feed(p, frames, 185, frames.size()).reference, ReferenceState::Lost);
}

TEST(PipelineReference, ResetDropsALostReference) {
  GrainPipeline p{GrainOptions{}};
  ASSERT_EQ(walk_until_lost(p).reference, ReferenceState::Lost);
  p.reset();
  EXPECT_FALSE(p.reference_mark().has_value());
  const auto frames = walking_frames();
  const GrainReport r = feed(p, frames, 200, frames.size());
  EXPECT_EQ(r.reference, ReferenceState::None);
  for (const GrainView& g : r.grains) {
    EXPECT_FALSE(g.reference);
    EXPECT_EQ(g.label, Label::None);
  }
}

TEST(PipelineReference, ResetInAGapDropsTheReference) {
  const auto frames = with_gap(kRefIndex, 80, 150);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  std::size_t i = kClickFrame;
  GrainReport r;
  do {
    r = p.process(frames[i++]);
  } while (r.reference != ReferenceState::Lost && i < 150);
  ASSERT_EQ(r.reference, ReferenceState::Lost);
  ASSERT_NE(grain_at(r, kRef), nullptr) << "track still waiting";
  p.reset();
  EXPECT_FALSE(p.reference_mark().has_value());
}

TEST(PipelineReference, ResetDropsAReferenceCutByTheEdge) {
  const auto frames = walking_frames();
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kWalkRef, Side::Below).has_value());
  // One jump walks it onto the border; the ring refills in 8 frames.
  const auto after_jump = static_cast<std::size_t>(kFirstJump * kFps) + 9;
  const GrainReport r = feed(p, frames, kClickFrame, after_jump);
  const auto cut = std::find_if(r.grains.begin(), r.grains.end(), [](const GrainView& g) { return g.reference; });
  ASSERT_NE(cut, r.grains.end());
  ASSERT_TRUE(cut->at_edge);
  ASSERT_EQ(r.reference, ReferenceState::Lost);
  p.reset();
  EXPECT_FALSE(p.reference_mark().has_value());
}

TEST(PipelineReference, ResetKeepsTheFirstWhenOnlyTheSecondIsLost) {
  const auto frames = with_gap(kLowIndex, 60, 160);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(p.set_second_reference(kLow, Side::Below).has_value());
  feed(p, frames, kClickFrame, 150);
  p.reset();
  EXPECT_TRUE(p.reference_mark().has_value());
  EXPECT_FALSE(p.second_reference_mark().has_value());
}

TEST(PipelineReference, ResetDropsTheSecondWithTheFirst) {
  const auto frames = with_gap(kRefIndex, 60, 160);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(p.set_second_reference(kLow, Side::Below).has_value());
  feed(p, frames, kClickFrame, 150);
  p.reset();
  EXPECT_FALSE(p.reference_mark().has_value());
  EXPECT_FALSE(p.second_reference_mark().has_value());
}

TEST(PipelineReference, ResetKeepsReferenceAsMark) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  const GrainReport voted = feed(p, frames, kClickFrame, 100);
  ASSERT_GT(total_passes(voted), 0);

  p.reset();
  ASSERT_TRUE(p.reference_mark().has_value());
  EXPECT_EQ(p.reference_mark()->side, Side::Below);

  const GrainReport bound = feed(p, frames, 100, 100 + kClickFrame);
  EXPECT_TRUE(bound.reference == ReferenceState::MoveFocus || bound.reference == ReferenceState::Reading);
  const GrainView* reference = grain_at(bound, kRef);
  ASSERT_NE(reference, nullptr);
  EXPECT_TRUE(reference->reference);
  EXPECT_EQ(total_passes(bound), 0);
  for (const GrainView& g : bound.grains) EXPECT_LE(g.id, 4U) << "ids restart";

  EXPECT_GT(total_passes(feed(p, frames, 100 + kClickFrame, frames.size())), 0);
}

TEST(PipelineReference, RestoredMarkBindsWithoutContrastCheck) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  p.restore_references({kNear, Side::Below}, std::nullopt);
  const GrainReport r = feed(p, frames, 0, kClickFrame + 8);
  EXPECT_NE(r.reference, ReferenceState::Lost);
  const GrainView* reference = grain_at(r, kNear);
  ASSERT_NE(reference, nullptr);
  EXPECT_TRUE(reference->reference);
}

TEST(PipelineReference, ClearReferenceDropsLabelsAndVotes) {
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline p{GrainOptions{}};
  feed(p, frames, 0, kClickFrame);
  ASSERT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  ASSERT_GT(total_passes(feed(p, frames, kClickFrame, 100)), 0);
  p.clear_reference();
  const GrainReport r = feed(p, frames, 100, 101);
  EXPECT_EQ(r.reference, ReferenceState::None);
  EXPECT_FALSE(p.reference_mark().has_value());
  EXPECT_EQ(total_passes(r), 0);
  for (const GrainView& g : r.grains) EXPECT_EQ(g.label, Label::None);
}
