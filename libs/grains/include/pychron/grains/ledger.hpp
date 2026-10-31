#pragma once

#include <cstdint>
#include <map>
#include <span>

#include "pychron/grains/score.hpp"
#include "pychron/grains/types.hpp"

namespace pychron::grains {

// Turns fringe velocities into votes against the reference. A pass is a run
// of frames in which the reference moves one way at or above
// min_ref_speed_px_s; when it closes, each grain seen in it votes Same or
// Opposite by whether it moved with the reference, or None when its fringe was
// too faint. Only the sign of the product of displacements matters, so a
// reversal of the focus direction cannot change a vote.
class PassLedger {
 public:
  explicit PassLedger(const GrainOptions& options);

  struct Sample {
    std::uint64_t id;
    double velocity_px_s;
    double contrast;
  };

  void update(TimePoint t, double reference_velocity, double reference_contrast,
              std::span<const Sample> grains);
  void drop_pass();  // throw away the pass in progress
  void forget(std::uint64_t id);
  [[nodiscard]] Tally tally(std::uint64_t id) const;  // zeros for an unknown id
  [[nodiscard]] Vote last_vote(std::uint64_t id) const;
  [[nodiscard]] bool pass_running() const { return frames_ > 0; }
  void reset();

 private:
  struct Accum {
    double displacement = 0;
    double contrast_sum = 0;
    int frames = 0;
  };
  struct Record {
    Tally tally;
    Vote last = Vote::None;
  };

  void close_pass();

  GrainOptions options_;
  bool have_last_ = false;
  TimePoint last_t_{};
  int sign_ = 0;
  int frames_ = 0;
  double ref_displacement_ = 0;
  double ref_contrast_sum_ = 0;
  std::map<std::uint64_t, Accum> pass_;
  std::map<std::uint64_t, Record> records_;
};

}  // namespace pychron::grains
