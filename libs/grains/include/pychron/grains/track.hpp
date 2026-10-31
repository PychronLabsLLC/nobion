#pragma once

#include <cstdint>
#include <vector>

#include "pychron/grains/types.hpp"
#include "pychron/vision/kernel.hpp"

namespace pychron::grains {

struct Track {
  std::uint64_t id = 0;
  vision::Component blob;
  int missed = 0;  // frames since last matched; 0: matched this frame
};

// Follows blobs from frame to frame by bounding-box overlap (intersection over
// union of at least 0.3). A blob that overlaps two tracks, or a track that
// overlaps two blobs, is a merge or a split: the tracks involved end and the
// blobs start new ones, since a label earned by one grain is not the other's.
// A track not matched is kept for track_gap_frames frames. Ids start at 1 and
// are not reused until reset().
class Tracker {
 public:
  explicit Tracker(const GrainOptions&);

  // Live tracks after this frame, matched or still waiting (missed > 0).
  const std::vector<Track>& update(std::vector<vision::Component> blobs);
  std::vector<std::uint64_t> take_ended();  // ids ended since last asked
  void reset();

 private:
  GrainOptions options_;
  std::vector<Track> tracks_;
  std::vector<std::uint64_t> ended_;
  std::uint64_t next_id_ = 1;
};

}  // namespace pychron::grains
