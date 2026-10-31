#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/grains/ledger.hpp"
#include "pychron/grains/segment.hpp"
#include "pychron/grains/track.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::grains {

// One frame in, one report out. Single-threaded, with no clock of its own: the
// frame's timestamp is all the time it sees, so a given run of frames always
// gives the same reports.
class GrainPipeline {
 public:
  explicit GrainPipeline(GrainOptions options);

  GrainReport process(const vision::Frame& frame);

  // ErrorKind::Config, naming the reason, when no grain is at the point, it is
  // cut by the frame edge, its recent contrast is under
  // min_reference_contrast, or it is the other reference. Votes are relative
  // to the grain: the same grain clicked again keeps them, and a changed side
  // inverts Candidate and Not at once. Another grain clears them.
  Result<void> set_reference(vision::Vec2 frame_px, Side side);
  // Needs the first reference. Tallied against it as any grain is: expected
  // Opposite when the sides differ, Same when equal.
  Result<void> set_second_reference(vision::Vec2 frame_px, Side side);
  void clear_reference();

  // Clears tracks and votes. A reference tracked at this moment (no gap, not
  // cut by the edge) stays as a mark, bound again to the blob at its place
  // without a contrast check; any other is dropped, since what sits there now
  // may be another grain. The second goes with the first. Ids restart and
  // GrainReport::generation moves on. process() does this itself on a frame of
  // another size or depth, or a blank one, and then drops both references and
  // warns until one is set.
  void reset();
  // For a replay: as reset() left the marks, bound the same way.
  void restore_references(ReferenceMark first, std::optional<ReferenceMark> second);

  // A mark follows its grain while it is tracked.
  [[nodiscard]] std::optional<ReferenceMark> reference_mark() const;
  [[nodiscard]] std::optional<ReferenceMark> second_reference_mark() const;

 private:
  struct Reference {
    ReferenceMark mark;
    std::optional<std::uint64_t> id;
    bool binding = false;  // waiting for a blob at the mark
  };
  struct Fringe {
    std::vector<std::pair<TimePoint, double>> positions;  // oldest first
    std::vector<double> contrasts;
    double velocity = 0;
    double contrast = 0;
  };

  [[nodiscard]] bool camera_too_slow(TimePoint t);
  void measure_fringes(const vision::FrameView& view);
  void end_tracks();
  void bind(std::optional<Reference>& reference, const std::optional<Reference>& other);
  void follow(std::optional<Reference>& reference);
  void vote(TimePoint t, bool may_vote);
  [[nodiscard]] Result<std::uint64_t> pick(vision::Vec2 at, const std::optional<Reference>& other) const;
  [[nodiscard]] const Track* track(const std::optional<Reference>& reference) const;
  [[nodiscard]] bool usable(const Track* reference_track) const;
  [[nodiscard]] bool disagreeing() const;
  [[nodiscard]] ReferenceState state() const;
  [[nodiscard]] GrainView view_of(const Track& track) const;

  GrainOptions options_;
  Segmenter segmenter_;
  Tracker tracker_;
  PassLedger ledger_;
  std::map<std::uint64_t, Fringe> fringes_;
  std::optional<Reference> first_, second_;
  bool disagree_held_ = false;  // the second reference ended in disagreement
  bool blind_ = false;           // the segmenter is warming up: tracks are stale
  bool camera_changed_ = false;  // references dropped, none set since
  std::uint64_t generation_ = 0;
  vision::Frame previous_;
  std::deque<TimePoint> times_;
};

}  // namespace pychron::grains
