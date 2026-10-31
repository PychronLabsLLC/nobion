#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/vision/types.hpp"

namespace pychron::grains {

enum class Side { Below, Above };                   // relative to oil
enum class Label { None, Candidate, Not, Unsure };  // None: no reference yet
enum class Vote { Same, Opposite, None };
enum class ReferenceState { None, Lost, MoveFocus, Reading, Disagree };

struct ReferenceMark {
  vision::Vec2 at;
  Side side = Side::Below;
};

struct GrainOptions {
  double min_contrast = 0.01;  // fraction of pixel_depth
  double min_reference_contrast = 0.03;
  int min_passes = 3;
  double min_score = 0.8;
  int track_gap_frames = 15;
  double max_shift_px = 8.0;
  int disagree_passes = 2;
  double min_fps = 10.0;
  int segment_window = 15;  // frames of gradient accumulated
  int ring_px = 12;         // radial reach each side of the outline
  int min_area_px = 400;
  int min_pass_frames = 5;
  double min_ref_speed_px_s = 1.0;  // below this the reference is not moving
};

struct GrainView {
  std::uint64_t id = 0;
  std::vector<vision::Vec2> outline;
  double fringe_velocity_px_s = 0;  // positive outward
  double contrast = 0;              // fraction of pixel_depth
  Label label = Label::None;
  double score = 0;  // not a probability
  int passes_agree = 0, passes_against = 0, passes_none = 0;
  bool reference = false;
  bool at_edge = false;
  Vote last_vote = Vote::None;
};

struct GrainReport {
  std::uint64_t seq = 0;
  TimePoint timestamp;
  ReferenceState reference = ReferenceState::None;
  std::vector<GrainView> grains;
  std::string warning;  // empty: none
};

}  // namespace pychron::grains
