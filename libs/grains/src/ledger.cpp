#include "pychron/grains/ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pychron::grains {

PassLedger::PassLedger(const GrainOptions& options) : options_(options) {}

void PassLedger::update(TimePoint t, double reference_velocity, double reference_contrast,
                        std::span<const Sample> grains) {
  const double dt = have_last_ ? std::chrono::duration<double>(t - last_t_).count() : 0.0;
  have_last_ = true;
  last_t_ = t;

  const bool moving = std::abs(reference_velocity) >= options_.min_ref_speed_px_s;
  const int sign = reference_velocity < 0 ? -1 : 1;
  if (frames_ > 0 && (!moving || sign != sign_)) {
    close_pass();
  }
  if (!moving) {
    return;
  }
  if (frames_ == 0) {
    sign_ = sign;
  }

  ++frames_;
  ref_displacement_ += reference_velocity * dt;
  ref_contrast_sum_ += reference_contrast;
  for (const Sample& g : grains) {
    Accum& a = pass_[g.id];
    a.displacement += g.velocity_px_s * dt;
    a.contrast_sum += g.contrast;
    ++a.frames;
  }
}

void PassLedger::close_pass() {
  if (frames_ >= options_.min_pass_frames) {
    const double ref_contrast = ref_contrast_sum_ / frames_;
    for (const auto& [id, a] : pass_) {
      const double contrast = a.contrast_sum / a.frames;
      Record& r = records_[id];
      const double product = a.displacement * ref_displacement_;
      if (contrast < options_.min_contrast || product == 0.0) {
        ++r.tally.none_passes;
        r.last = Vote::None;
        continue;
      }
      const double weight = ref_contrast > 0 ? std::min(1.0, contrast / ref_contrast) : 1.0;
      if (product > 0) {
        r.tally.same_weight += weight;
        ++r.tally.same_passes;
        r.last = Vote::Same;
      } else {
        r.tally.opposite_weight += weight;
        ++r.tally.opposite_passes;
        r.last = Vote::Opposite;
      }
    }
  }
  drop_pass();
}

void PassLedger::drop_pass() {
  frames_ = 0;
  sign_ = 0;
  ref_displacement_ = 0;
  ref_contrast_sum_ = 0;
  pass_.clear();
}

void PassLedger::forget(std::uint64_t id) {
  records_.erase(id);
  pass_.erase(id);
}

Tally PassLedger::tally(std::uint64_t id) const {
  const auto it = records_.find(id);
  return it == records_.end() ? Tally{} : it->second.tally;
}

Vote PassLedger::last_vote(std::uint64_t id) const {
  const auto it = records_.find(id);
  return it == records_.end() ? Vote::None : it->second.last;
}

void PassLedger::reset() {
  drop_pass();
  records_.clear();
  have_last_ = false;
}

}  // namespace pychron::grains
