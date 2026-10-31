#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pipeline_scene.hpp"
#include "pychron/grains/recording.hpp"

using namespace pychron::grains;
using namespace pychron::grains::scene;

namespace {

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    path = std::filesystem::temp_directory_path() / ("pychron_grains_recording_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

using Events = std::map<std::size_t, std::vector<RecordEvent>>;

// Frames [from, to) through `p`, recorded into `dir`; `events` are applied to
// `p` and recorded just before the frame with their index. Returns the last
// report, and ADD_FAILUREs when a call fails.
GrainReport record(GrainPipeline& p, const std::filesystem::path& dir, const GrainOptions& options, const std::vector<Frame>& frames, std::size_t from,
                   std::size_t to, const Events& events = {}, std::vector<GrainReport>* all = nullptr) {
  GrainReport last;
  auto rec = GrainRecording::open(dir, options);
  if (!rec) {
    ADD_FAILURE() << rec.error().what;
    return last;
  }
  for (std::size_t i = from; i < to; ++i) {
    std::vector<RecordEvent> applied;
    if (const auto it = events.find(i); it != events.end()) {
      applied = it->second;
      for (const RecordEvent& e : applied) {
        switch (e.kind) {
          case RecordEvent::Kind::Set:
            EXPECT_TRUE(p.set_reference(e.first.at, e.first.side).has_value());
            break;
          case RecordEvent::Kind::SetSecond:
            EXPECT_TRUE(p.set_second_reference(e.first.at, e.first.side).has_value());
            break;
          case RecordEvent::Kind::Clear: p.clear_reference(); break;
          case RecordEvent::Kind::Restore: p.restore_references(e.first, e.second); break;
        }
      }
    }
    last = p.process(frames[i]);
    if (all != nullptr) all->push_back(last);
    const auto added = rec->add(frames[i], last, applied);
    if (!added) {
      ADD_FAILURE() << added.error().what;
      return last;
    }
  }
  const auto done = rec->finish();
  if (!done) ADD_FAILURE() << done.error().what;
  return last;
}

// The first kClickFrame frames are unrecorded; the reference is set, then the
// pipeline reset, so the recording starts as a replay will.
GrainPipeline primed(const GrainOptions& options, const std::vector<Frame>& frames, std::vector<RecordEvent>& restore) {
  GrainPipeline p(options);
  feed(p, frames, 0, kClickFrame);
  EXPECT_TRUE(p.set_reference(kRef, Side::Below).has_value());
  RecordEvent e;
  e.kind = RecordEvent::Kind::Restore;
  e.first = *p.reference_mark();
  e.second = p.second_reference_mark();
  restore.push_back(e);
  p.reset();
  return p;
}

void expect_same(const std::vector<GrainView>& a, const std::vector<GrainView>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].id, b[i].id);
    EXPECT_EQ(a[i].label, b[i].label);
    EXPECT_NEAR(a[i].fringe_velocity_px_s, b[i].fringe_velocity_px_s, 1e-9);
    EXPECT_EQ(a[i].passes_agree, b[i].passes_agree);
  }
}

constexpr std::size_t kLast = 90;  // frames kept, a few seconds past the click

}  // namespace

TEST(Recording, ReplayEqualsLive) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  const GrainOptions options;
  std::vector<RecordEvent> restore;
  GrainPipeline live = primed(options, frames, restore);
  const GrainReport last = record(live, dir.path, options, frames, kClickFrame, kLast, {{kClickFrame, restore}});
  ASSERT_FALSE(last.grains.empty());

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->frames, kLast - kClickFrame);
  EXPECT_EQ(r->mismatches, 0U);
  expect_same(r->final_grains, last.grains);
  EXPECT_NE(summary_text(*r).find("mismatches 0"), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(dir.path / "tracks.jsonl"));
  EXPECT_TRUE(std::filesystem::exists(dir.path / "case.toml"));
}

TEST(Recording, RestoreWithASecondMarkReplays) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  const GrainOptions options;
  GrainPipeline live(options);
  feed(live, frames, 0, kClickFrame);
  ASSERT_TRUE(live.set_reference(kRef, Side::Below).has_value());
  ASSERT_TRUE(live.set_second_reference(kHigh, Side::Above).has_value());
  live.reset();
  ASSERT_TRUE(live.second_reference_mark().has_value());
  RecordEvent restore;
  restore.kind = RecordEvent::Kind::Restore;
  restore.first = *live.reference_mark();
  restore.second = live.second_reference_mark();

  const GrainReport last = record(live, dir.path, options, frames, kClickFrame, 120, {{kClickFrame, {restore}}});
  const auto reference_flags = std::count_if(last.grains.begin(), last.grains.end(), [](const GrainView& g) { return g.reference; });
  EXPECT_EQ(reference_flags, 2);

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->mismatches, 0U);
  expect_same(r->final_grains, last.grains);
}

TEST(Recording, RecordingStartedAfterACameraChangeReplaysClean) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainScene small = four_grains(kSanidine);
  small.width = 300;
  small.height = 230;
  const auto other = render(small, triangle_z(2, 1), kFps, 30);

  const GrainOptions options;
  GrainPipeline live(options);
  feed(live, frames, 0, kClickFrame);
  ASSERT_TRUE(live.set_reference(kRef, Side::Below).has_value());
  feed(live, frames, kClickFrame, 20);
  ASSERT_FALSE(live.process(other[0]).warning.empty());

  // No click: the recording starts with nothing a replay could not rebuild.
  live.reset();
  const GrainReport last = record(live, dir.path, options, other, 1, other.size());
  EXPECT_TRUE(last.warning.empty()) << last.warning;

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->frames, other.size() - 1);
  EXPECT_EQ(r->mismatches, 0U);
}

TEST(Recording, ReplayKeepsTheRecordedOptions) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainOptions options;
  options.min_passes = 1;
  options.min_score = 0.1;
  std::vector<RecordEvent> restore;
  GrainPipeline live = primed(options, frames, restore);
  record(live, dir.path, options, frames, kClickFrame, kLast, {{kClickFrame, restore}});

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->mismatches, 0U);

  // The recorded options are what ran: under the defaults the same frames give
  // other labels, so a replay that dropped them would not match.
  GrainPipeline plain{GrainOptions{}};
  plain.restore_references(*live.reference_mark(), std::nullopt);
  GrainReport defaults;
  for (std::size_t i = kClickFrame; i < kLast; ++i) defaults = plain.process(frames[i]);
  bool differs = defaults.grains.size() != r->final_grains.size();
  for (std::size_t i = 0; !differs && i < defaults.grains.size(); ++i) differs = defaults.grains[i].label != r->final_grains[i].label;
  EXPECT_TRUE(differs) << "scene does not tell min_passes 1 from 3";
}

TEST(Recording, ReplayKeepsFrameTimes) {
  TempDir dir;
  auto frames = sweep_frames(kSanidine);
  // Later and slower than the synth's own stamps: a replay stamped at grab
  // would see frames microseconds apart.
  for (std::size_t i = 0; i < frames.size(); ++i)
    frames[i].timestamp = pychron::TimePoint{} + std::chrono::hours(2) + std::chrono::milliseconds(static_cast<long long>(i) * 90);
  const GrainOptions options;
  std::vector<RecordEvent> restore;
  GrainPipeline live = primed(options, frames, restore);
  const GrainReport last = record(live, dir.path, options, frames, kClickFrame, kLast, {{kClickFrame, restore}});

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->mismatches, 0U);
  bool moving = false;
  for (const GrainView& g : last.grains) moving = moving || g.fringe_velocity_px_s != 0;
  EXPECT_TRUE(moving);
  expect_same(r->final_grains, last.grains);
}

TEST(Recording, ReferenceClickMidwayReplays) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  const GrainOptions options;
  GrainPipeline live(options);
  RecordEvent set;
  set.kind = RecordEvent::Kind::Set;
  set.first = ReferenceMark{kRef, Side::Below};
  RecordEvent clear;
  clear.kind = RecordEvent::Kind::Clear;
  RecordEvent again = set;
  again.first.side = Side::Above;
  const Events events{{kClickFrame, {set}}, {50, {clear}}, {60, {again}}};
  const GrainReport last = record(live, dir.path, options, frames, 0, kLast, events);

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->frames, kLast);
  EXPECT_EQ(r->mismatches, 0U);
  expect_same(r->final_grains, last.grains);
}

TEST(Recording, CorruptSidecarIsAnError) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline live{GrainOptions{}};
  record(live, dir.path, GrainOptions{}, frames, 0, 3);

  std::string text;
  {
    std::ifstream in(dir.path / "tracks.jsonl", std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(text.size(), 100U);
  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << text.substr(0, text.size() - 40);  // the last line cut short
  }
  const auto r = replay_folder(dir.path);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("tracks.jsonl:"), std::string::npos) << r.error().what;

  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << "not json\n";
  }
  EXPECT_FALSE(replay_folder(dir.path).has_value());
}

TEST(Recording, SidecarCutToItsHeaderIsAnError) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline live{GrainOptions{}};
  record(live, dir.path, GrainOptions{}, frames, 0, 4);
  std::string text;
  {
    std::ifstream in(dir.path / "tracks.jsonl", std::ios::binary);
    std::getline(in, text);
  }
  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << text << '\n';
  }
  const auto r = replay_folder(dir.path);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("case.toml"), std::string::npos) << r.error().what;
}

TEST(Recording, TamperedOptionsAreAnError) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline live{GrainOptions{}};
  record(live, dir.path, GrainOptions{}, frames, 0, 3);
  std::string text;
  {
    std::ifstream in(dir.path / "tracks.jsonl", std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::string key = R"("min_passes":3)";
  const auto at = text.find(key);
  ASSERT_NE(at, std::string::npos);
  text.replace(at, key.size(), R"("min_passes":0)");
  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << text;
  }
  const auto r = replay_folder(dir.path);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().what.find("min_passes"), std::string::npos) << r.error().what;
}

TEST(Recording, FewerLinesThanFramesIsAnError) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline live{GrainOptions{}};
  record(live, dir.path, GrainOptions{}, frames, 0, 4);
  std::string text;
  {
    std::ifstream in(dir.path / "tracks.jsonl", std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  text.erase(text.rfind('\n', text.size() - 2) + 1);  // drop the last line whole
  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << text;
  }
  EXPECT_FALSE(replay_folder(dir.path).has_value());
}

TEST(Recording, MissingFolderIsAnError) {
  TempDir dir;
  const auto r = replay_folder(dir.path / "nowhere");
  ASSERT_FALSE(r.has_value());
  EXPECT_FALSE(r.error().what.empty());
}

TEST(Recording, EmptyRecordingReplaysToZeroFrames) {
  TempDir dir;
  auto rec = GrainRecording::open(dir.path / "empty", GrainOptions{});
  ASSERT_TRUE(rec.has_value()) << (rec ? "" : rec.error().what);
  const auto done = rec->finish();
  ASSERT_TRUE(done.has_value()) << (done ? "" : done.error().what);
  const auto r = replay_folder(dir.path / "empty");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->frames, 0U);
  EXPECT_EQ(r->mismatches, 0U);
  EXPECT_TRUE(r->final_grains.empty());
}

TEST(Recording, ADifferentReportIsAMismatch) {
  TempDir dir;
  const auto frames = sweep_frames(kSanidine);
  GrainPipeline live{GrainOptions{}};
  std::vector<GrainReport> reports;
  record(live, dir.path, GrainOptions{}, frames, 0, 30, {}, &reports);
  std::string text;
  {
    std::ifstream in(dir.path / "tracks.jsonl", std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  // Change the recorded state of the last frame.
  const std::string key = R"("state":")";
  const auto at = text.rfind(key);
  ASSERT_NE(at, std::string::npos);
  text.insert(at + key.size(), "x");
  {
    std::ofstream out(dir.path / "tracks.jsonl", std::ios::binary | std::ios::trunc);
    out << text;
  }
  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->mismatches, 1U);
}
