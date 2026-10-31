#pragma once

#include <string_view>

#include "pychron/grains/types.hpp"

namespace pychron::grains {

struct Tally {
  double same_weight = 0, opposite_weight = 0;
  int same_passes = 0, opposite_passes = 0, none_passes = 0;
};

// (agree + 1) / (agree + against + 2): evidence, not a probability.
[[nodiscard]] double evidence_score(double agree, double against);

// reference_side is that of the reference the votes were cast against.
[[nodiscard]] Label decide(const Tally& tally, Side reference_side, const GrainOptions& options);

[[nodiscard]] std::string_view label_text(Label label);  // "sanidine?", "not", "unsure", ""

}  // namespace pychron::grains
