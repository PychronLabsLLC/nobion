#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/grains/pipeline.hpp"
#include "pychron/grains/recording.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::grains {

struct FrameTick {
  std::optional<vision::Frame> frame;
  std::string error;  // empty: frames arriving
};

// The worker thread round a GrainPipeline. Real time by design: no simulated
// path, so no Clock::Participant; the thread polls on a short real interval.
//
// Only the worker touches the pipeline and the recording. Commands queue and
// run on the worker between frames, so a command is applied before the next
// frame is processed (and, when recording, logged on that frame's line).
//
// `poll` runs on the worker and must return quickly (LiveFeed::latest does not
// block). A frame whose seq was already processed is skipped. `on_report`
// carries nothing and runs on the worker: the taker reads take_report(), and
// marshals to its own thread itself. The `done` callbacks of the set_reference
// calls also run on the worker, so their callers marshal back themselves. No
// callback is called with an internal mutex held.
class GrainRunner {
 public:
  GrainRunner(std::function<FrameTick()> poll, GrainOptions options, std::function<void()> on_report);
  // Finishes an open recording, then joins. Prompt even when poll keeps
  // returning new frames.
  ~GrainRunner();
  GrainRunner(const GrainRunner&) = delete;
  GrainRunner& operator=(const GrainRunner&) = delete;
  GrainRunner(GrainRunner&&) = delete;
  GrainRunner& operator=(GrainRunner&&) = delete;

  // What the worker last made: a frame and its report, or an error from poll
  // (no frame, report empty). A new error is shown once, not on every tick.
  struct Shown {
    std::optional<vision::Frame> frame;
    GrainReport report;
    std::string error;
  };
  // The newest; older ones are dropped. Empty when nothing new since the last
  // call.
  [[nodiscard]] std::optional<Shown> take_report();

  void set_reference(vision::Vec2 frame_px, Side side, std::function<void(Result<void>)> done);
  void set_second_reference(vision::Vec2 frame_px, Side side, std::function<void(Result<void>)> done);
  void clear_reference();

  // Both wait for the worker to run them and return its result. Start resets
  // the pipeline on the worker and logs the marks held as a Restore event.
  // Stop returns the first error the recording met, if any.
  [[nodiscard]] Result<void> start_recording(const std::filesystem::path& dir);
  [[nodiscard]] Result<void> stop_recording();

 private:
  void run();
  void post(Shown shown);

  std::function<FrameTick()> poll_;
  GrainOptions options_;
  std::function<void()> on_report_;

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::vector<std::function<void()>> commands_;  // guarded by mutex_
  std::optional<Shown> mailbox_;                 // guarded by mutex_

  // Worker only.
  GrainPipeline pipeline_;
  std::optional<GrainRecording> recording_;
  std::optional<Error> recording_error_;
  std::vector<RecordEvent> events_;
  std::optional<std::uint64_t> last_seq_;
  std::string last_error_;

  std::thread thread_;  // last: starts when the rest exists
};

}  // namespace pychron::grains
