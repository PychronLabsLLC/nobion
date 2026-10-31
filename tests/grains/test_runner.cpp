#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pipeline_scene.hpp"
#include "pychron/grains/recording.hpp"
#include "pychron/grains/runner.hpp"

using namespace pychron::grains;
using namespace pychron::grains::scene;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLimit = 10s;

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    path = std::filesystem::temp_directory_path() / ("pychron_grains_runner_" + std::to_string(Clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
    std::filesystem::remove_all(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// Polls take_report until `pred` accepts one, or the limit passes.
template <class Pred>
std::optional<GrainRunner::Shown> wait_for_shown(GrainRunner& runner, Pred pred) {
  const auto deadline = Clock::now() + kLimit;
  while (Clock::now() < deadline) {
    if (auto s = runner.take_report(); s && pred(*s)) return s;
    std::this_thread::sleep_for(1ms);
  }
  return std::nullopt;
}

// The frame at `limit` is the newest the camera has; the same one again until
// the test moves it on.
struct Feed {
  std::vector<Frame> frames;
  std::atomic<std::size_t> limit{0};
  std::atomic<int> polls{0};
  std::function<FrameTick()> poll() {
    return [this] {
      ++polls;
      FrameTick t;
      t.frame = frames[limit.load()];
      return t;
    };
  }
  // Moves the camera on to frame i and waits for its report.
  std::optional<GrainRunner::Shown> step(GrainRunner& runner, std::size_t i) {
    limit = i;
    return wait_for_shown(runner, [&](const GrainRunner::Shown& s) { return s.frame && s.frame->seq == frames[i].seq; });
  }
};

}  // namespace

TEST(Runner, ProducesReports) {
  Feed feed;
  feed.frames = sweep_frames(kSanidine);
  std::atomic<int> woken{0};
  GrainRunner runner(feed.poll(), GrainOptions{}, [&] { ++woken; });
  for (std::size_t i = 0; i < 5; ++i) {
    const auto s = feed.step(runner, i);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->report.seq, feed.frames[i].seq);
    EXPECT_TRUE(s->error.empty());
    EXPECT_EQ(s->frame->width, 320);
  }
  EXPECT_GE(woken.load(), 5);
}

TEST(Runner, SameSeqNotProcessedTwice) {
  Feed feed;
  feed.frames = sweep_frames(kSanidine);
  std::atomic<int> reports{0};
  GrainRunner runner(feed.poll(), GrainOptions{}, [&] { ++reports; });
  ASSERT_TRUE(feed.step(runner, 0).has_value());
  const int polls_then = feed.polls.load();
  const auto deadline = Clock::now() + kLimit;
  while (feed.polls.load() < polls_then + 10 && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
  ASSERT_GE(feed.polls.load(), polls_then + 10);
  EXPECT_EQ(reports.load(), 1);
  EXPECT_FALSE(runner.take_report().has_value());
}

TEST(Runner, SlowTakerGetsNewestOnly) {
  Feed feed;
  feed.frames = sweep_frames(kSanidine);
  std::atomic<int> reports{0};
  GrainRunner runner(feed.poll(), GrainOptions{}, [&] { ++reports; });
  for (std::size_t i = 0; i < 6; ++i) {
    feed.limit = i;
    const auto deadline = Clock::now() + kLimit;
    while (reports.load() < static_cast<int>(i) + 1 && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    ASSERT_GE(reports.load(), static_cast<int>(i) + 1);
  }
  const auto s = runner.take_report();
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->report.seq, feed.frames[5].seq);
  EXPECT_FALSE(runner.take_report().has_value());
}

TEST(Runner, ErrorFromPollIsShownAndFramesResume) {
  const auto frames = sweep_frames(kSanidine);
  std::atomic<bool> failing{false};
  std::atomic<std::size_t> next{0};
  std::atomic<int> error_ticks{0};
  GrainRunner runner(
      [&] {
        FrameTick t;
        if (failing) {
          ++error_ticks;
          t.error = "camera lost";
          return t;
        }
        t.frame = frames[next.load()];
        return t;
      },
      GrainOptions{}, [] {});
  ASSERT_TRUE(wait_for_shown(runner, [](const GrainRunner::Shown& s) { return s.frame.has_value(); }).has_value());
  failing = true;
  const auto err = wait_for_shown(runner, [](const GrainRunner::Shown& s) { return !s.error.empty(); });
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->error, "camera lost");
  EXPECT_FALSE(err->frame.has_value());
  // The same error is not shown again on every tick.
  const int ticks = error_ticks.load();
  const auto deadline = Clock::now() + kLimit;
  while (error_ticks.load() < ticks + 5 && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
  ASSERT_GE(error_ticks.load(), ticks + 5);
  EXPECT_FALSE(runner.take_report().has_value());

  next = 1;
  failing = false;
  const auto back = wait_for_shown(runner, [](const GrainRunner::Shown& s) { return s.frame.has_value(); });
  ASSERT_TRUE(back.has_value());
  EXPECT_TRUE(back->error.empty());
  EXPECT_EQ(back->report.seq, frames[1].seq);
}

TEST(Runner, ReferenceDoneCalledWithRefusal) {
  Feed feed;
  feed.frames = sweep_frames(kSanidine);
  GrainRunner runner(feed.poll(), GrainOptions{}, [] {});
  ASSERT_TRUE(feed.step(runner, 0).has_value());

  std::mutex m;
  std::condition_variable cv;
  std::optional<pychron::Result<void>> got;
  std::thread::id on;
  runner.set_reference({3, 3}, Side::Below, [&](pychron::Result<void> r) {
    const std::lock_guard lock(m);
    got = std::move(r);
    on = std::this_thread::get_id();
    cv.notify_all();
  });
  std::unique_lock lock(m);
  ASSERT_TRUE(cv.wait_for(lock, kLimit, [&] { return got.has_value(); }));
  ASSERT_FALSE(got->has_value());
  EXPECT_EQ(got->error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(on, std::this_thread::get_id());
}

TEST(Runner, RecordingStartsFresh) {
  TempDir dir;
  Feed feed;
  feed.frames = sweep_frames(kSanidine);
  GrainRunner runner(feed.poll(), GrainOptions{}, [] {});

  std::atomic<bool> set_ok{false};
  std::atomic<bool> set_done{false};
  for (std::size_t i = 0; i < static_cast<std::size_t>(kClickFrame); ++i) ASSERT_TRUE(feed.step(runner, i).has_value());
  runner.set_reference(kRef, Side::Below, [&](const pychron::Result<void>& r) {
    set_ok = r.has_value();
    set_done = true;
  });
  std::optional<GrainRunner::Shown> before;
  for (std::size_t i = kClickFrame; i < 110; ++i) {
    before = feed.step(runner, i);
    ASSERT_TRUE(before.has_value());
  }
  ASSERT_TRUE(set_done.load());
  ASSERT_TRUE(set_ok.load());
  int tallied = 0;
  for (const GrainView& g : before->report.grains) tallied += g.passes_agree + g.passes_against + g.passes_none;
  ASSERT_GT(tallied, 0) << "scene gave no passes to reset";

  // start_recording runs before the next frame: the camera holds still.
  ASSERT_TRUE(runner.start_recording(dir.path).has_value());
  const auto first = feed.step(runner, 110);
  ASSERT_TRUE(first.has_value());
  for (const GrainView& g : first->report.grains) EXPECT_EQ(g.passes_agree + g.passes_against + g.passes_none, 0);
  for (std::size_t i = 111; i < 140; ++i) ASSERT_TRUE(feed.step(runner, i).has_value());
  EXPECT_FALSE(runner.start_recording(dir.path).has_value());
  ASSERT_TRUE(runner.stop_recording().has_value());
  EXPECT_FALSE(runner.stop_recording().has_value());

  const auto r = replay_folder(dir.path);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().what);
  EXPECT_EQ(r->frames, 30U);
  EXPECT_EQ(r->mismatches, 0U);
}

TEST(Runner, DestructorJoinsDuringBusyPoll) {
  const auto frames = sweep_frames(kSanidine);
  std::atomic<std::uint64_t> seq{0};
  std::atomic<int> polls{0};
  auto runner = std::make_unique<GrainRunner>(
      [&] {
        ++polls;
        FrameTick t;
        t.frame = frames[0];
        t.frame->seq = ++seq;  // always a new frame
        return t;
      },
      GrainOptions{}, [] {});
  const auto deadline = Clock::now() + kLimit;
  while (polls.load() < 3 && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
  ASSERT_GE(polls.load(), 3);
  const auto start = Clock::now();
  runner.reset();
  EXPECT_LT(Clock::now() - start, 5s);
}
