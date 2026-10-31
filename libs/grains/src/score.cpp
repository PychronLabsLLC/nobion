#include "pychron/grains/score.hpp"

namespace pychron::grains {

double evidence_score(double agree, double against) {
  return (agree + 1.0) / (agree + against + 2.0);
}

Label decide(const Tally& tally, Side reference_side, const GrainOptions& options) {
  if (tally.same_passes + tally.opposite_passes < options.min_passes) return Label::Unsure;
  if (tally.same_weight == tally.opposite_weight) return Label::Unsure;

  const bool same_leads = tally.same_weight > tally.opposite_weight;
  const double agree = same_leads ? tally.same_weight : tally.opposite_weight;
  const double against = same_leads ? tally.opposite_weight : tally.same_weight;
  if (evidence_score(agree, against) < options.min_score) return Label::Unsure;

  const bool below = (reference_side == Side::Below) == same_leads;
  return below ? Label::Candidate : Label::Not;
}

std::string_view label_text(Label label) {
  switch (label) {
    case Label::Candidate: return "sanidine?";
    case Label::Not: return "not";
    case Label::Unsure: return "unsure";
    case Label::None: break;
  }
  return "";
}

}  // namespace pychron::grains
