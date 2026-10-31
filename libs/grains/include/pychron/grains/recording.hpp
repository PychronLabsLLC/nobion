#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/grains/pipeline.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::grains {

// An operator action on the pipeline. `first` is the mark of Set, of SetSecond
// (the second reference's mark) and of Restore; Restore also carries the
// second mark when there is one. Clear carries nothing.
struct RecordEvent {
  enum class Kind { Set, SetSecond, Clear, Restore } kind = Kind::Clear;
  ReferenceMark first;
  std::optional<ReferenceMark> second;
};

// A folder `replay_folder` can run again: one PGM per frame and case.toml
// (vision::FrameRecorder), and tracks.jsonl, the options and, per frame, its
// time, the events applied before it and the report it gave. Start it right
// after GrainPipeline::reset(), with a Restore event for the marks that survived it,
// so a replay's pipeline starts where the live one did.
// A recording not finished has no case.toml and does not replay.
class GrainRecording {
 public:
  [[nodiscard]] static Result<GrainRecording> open(const std::filesystem::path& dir, const GrainOptions& options);
  // `frame` is the one the report came from, `events` what was done to the
  // pipeline since the last frame.
  [[nodiscard]] Result<void> add(const vision::Frame& frame, const GrainReport& report, std::span<const RecordEvent> events);
  [[nodiscard]] Result<void> finish();

 private:
  GrainRecording(std::filesystem::path dir, std::ofstream out);

  std::filesystem::path dir_;
  vision::FrameRecorder recorder_;
  std::ofstream out_;
  std::size_t frames_ = 0;
  bool finished_ = false;
};

struct ReplaySummary {
  std::size_t frames = 0;
  std::size_t mismatches = 0;  // frames whose report differs from the recorded one
  std::vector<GrainView> final_grains;
};

// Runs the frames through a new pipeline with the recorded options, the
// recorded times and the recorded events. Errors: no folder, a sidecar that
// does not parse, or one that does not match the frames.
[[nodiscard]] Result<ReplaySummary> replay_folder(const std::filesystem::path& dir);

[[nodiscard]] std::string summary_text(const ReplaySummary& summary);

}  // namespace pychron::grains
