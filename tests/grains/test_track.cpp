#include <gtest/gtest.h>

#include <algorithm>

#include "pychron/grains/track.hpp"

using namespace pychron::grains;
using pychron::vision::Component;

namespace {

Component blob(int x, int y, int w, int h) {
  Component c;
  c.bbox = {x, y, w, h};
  c.area = static_cast<double>(w) * h;
  c.centroid = {x + w / 2.0, y + h / 2.0};
  return c;
}

bool has(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
  return std::ranges::find(ids, id) != ids.end();
}

}  // namespace

TEST(Track, IdStableAcrossFrames) {
  Tracker t({});
  const auto first = t.update({blob(100, 100, 80, 80)});
  ASSERT_EQ(first.size(), 1u);
  const auto id = first[0].id;
  const auto& second = t.update({blob(104, 102, 80, 80)});
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].id, id);
  EXPECT_EQ(second[0].missed, 0);
  EXPECT_EQ(second[0].blob.bbox.x, 104);
  EXPECT_TRUE(t.take_ended().empty());
}

TEST(Track, IdSurvivesAGapWithinTrackGapFrames) {
  GrainOptions o;
  o.track_gap_frames = 3;
  Tracker t(o);
  const auto id = t.update({blob(100, 100, 80, 80)})[0].id;
  for (int i = 1; i <= 3; ++i) {
    const auto& tracks = t.update({});
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_EQ(tracks[0].missed, i);
  }
  const auto& back = t.update({blob(101, 100, 80, 80)});
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0].id, id);
  EXPECT_EQ(back[0].missed, 0);
  EXPECT_TRUE(t.take_ended().empty());
}

TEST(Track, GapBeyondLimitEndsTrackAndNewBlobGetsNewId) {
  GrainOptions o;
  o.track_gap_frames = 3;
  Tracker t(o);
  const auto id = t.update({blob(100, 100, 80, 80)})[0].id;
  for (int i = 0; i < 4; ++i) t.update({});
  EXPECT_TRUE(t.update({}).empty());
  const auto ended = t.take_ended();
  ASSERT_EQ(ended.size(), 1u);
  EXPECT_EQ(ended[0], id);
  const auto& again = t.update({blob(100, 100, 80, 80)});
  ASSERT_EQ(again.size(), 1u);
  EXPECT_NE(again[0].id, id);
}

TEST(Track, MergedBlobIsANewTrack) {
  Tracker t({});
  const auto& two = t.update({blob(100, 100, 80, 80), blob(180, 100, 80, 80)});
  ASSERT_EQ(two.size(), 2u);
  const auto a = two[0].id;
  const auto b = two[1].id;
  const auto& one = t.update({blob(100, 100, 160, 80)});
  ASSERT_EQ(one.size(), 1u);
  EXPECT_NE(one[0].id, a);
  EXPECT_NE(one[0].id, b);
  const auto ended = t.take_ended();
  EXPECT_EQ(ended.size(), 2u);
  EXPECT_TRUE(has(ended, a));
  EXPECT_TRUE(has(ended, b));
}

TEST(Track, SplitBlobGivesTwoNewTracks) {
  Tracker t({});
  const auto id = t.update({blob(100, 100, 160, 80)})[0].id;
  const auto& two = t.update({blob(100, 100, 80, 80), blob(180, 100, 80, 80)});
  ASSERT_EQ(two.size(), 2u);
  EXPECT_NE(two[0].id, id);
  EXPECT_NE(two[1].id, id);
  EXPECT_NE(two[0].id, two[1].id);
  const auto ended = t.take_ended();
  ASSERT_EQ(ended.size(), 1u);
  EXPECT_EQ(ended[0], id);
}

TEST(Track, IdsNeverReused) {
  GrainOptions o;
  o.track_gap_frames = 0;
  Tracker t(o);
  std::vector<std::uint64_t> seen;
  for (int i = 0; i < 5; ++i) {
    const auto& tracks = t.update({blob(100, 100, 80, 80)});
    ASSERT_EQ(tracks.size(), 1u);
    EXPECT_FALSE(has(seen, tracks[0].id));
    seen.push_back(tracks[0].id);
    t.update({});
    t.update({});
  }
}

TEST(Track, ResetRestartsIdsFromOne) {
  Tracker t({});
  EXPECT_EQ(t.update({blob(100, 100, 80, 80)})[0].id, 1u);
  t.update({});
  t.reset();
  EXPECT_TRUE(t.take_ended().empty());
  EXPECT_EQ(t.update({blob(300, 300, 80, 80)})[0].id, 1u);
}
