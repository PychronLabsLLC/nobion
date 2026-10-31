#include "pychron/grains/pipeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <numeric>
#include <sstream>
#include <string>

#include "pychron/grains/fringe.hpp"
#include "pychron/grains/shift.hpp"

namespace pychron::grains {
namespace {

constexpr const char* kTooSlow = "camera too slow";
// Fringe samples a velocity is fitted over.
constexpr std::size_t kHistory = 5;
// Fewer give no slope.
constexpr std::size_t kMinHistory = 2;
// Frame intervals the measured rate is taken over.
constexpr std::size_t kRateWindow = 8;
constexpr int kSectors = 8;

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

// A point in a closed outline has outline in every direction from it; a point
// outside has none on its far side. The boundary is in raster order, so no
// polygon test is to hand.
bool contains(const vision::Component& c, vision::Vec2 at) {
  const vision::Rect& b = c.bbox;
  if (at.x < b.x || at.y < b.y || at.x >= b.x + b.w || at.y >= b.y + b.h) return false;
  std::array<bool, kSectors> seen{};
  for (const vision::Vec2& p : c.boundary) {
    const double dx = p.x - at.x, dy = p.y - at.y;
    if (dx == 0 && dy == 0) continue;
    const double a = std::atan2(dy, dx) + std::numbers::pi;  // [0, 2 pi]
    const int sector = std::min(kSectors - 1, static_cast<int>(a / (2 * std::numbers::pi) * kSectors));
    seen[static_cast<std::size_t>(sector)] = true;
  }
  return std::all_of(seen.begin(), seen.end(), [](bool s) { return s; });
}

// The boundary in order round the centroid, drawable as a polygon.
std::vector<vision::Vec2> ordered_outline(const vision::Component& c) {
  std::vector<vision::Vec2> out = c.boundary;
  auto angle = [&](const vision::Vec2& p) { return std::atan2(p.y - c.centroid.y, p.x - c.centroid.x); };
  std::stable_sort(out.begin(), out.end(), [&](const vision::Vec2& a, const vision::Vec2& b) { return angle(a) < angle(b); });
  return out;
}

}  // namespace

GrainPipeline::GrainPipeline(GrainOptions options)
    : options_(options), segmenter_(options_), tracker_(options_), ledger_(options_) {}

bool GrainPipeline::camera_too_slow(TimePoint t) {
  const bool gap = !times_.empty() && seconds(t - times_.back()) > 2.0 / options_.min_fps;
  times_.push_back(t);
  if (times_.size() > kRateWindow + 1) times_.pop_front();
  if (times_.size() < 2) return false;
  const double span = seconds(t - times_.front());
  return gap || (span > 0 && static_cast<double>(times_.size() - 1) / span < options_.min_fps);
}

void GrainPipeline::measure_fringes(const vision::FrameView& view) {
  for (const Track& t : tracker_.tracks()) {
    Fringe& f = fringes_[t.id];
    const FringeSample sample = t.missed == 0 ? measure_fringe(view, t.blob, options_.ring_px) : FringeSample{};
    if (!sample.valid) {
      f = Fringe{};
      continue;
    }
    if (f.positions.size() == kHistory) {
      f.positions.erase(f.positions.begin());
      f.contrasts.erase(f.contrasts.begin());
    }
    // From the centroid, not the outline: a fringe that lingers near the edge
    // moves the outline a little, and that is not the fringe moving.
    f.positions.emplace_back(view.timestamp, sample.radius_px);
    f.contrasts.push_back(sample.contrast);
    f.velocity = slope_px_s(f.positions);
    f.contrast = sample.contrast;
  }
}

void GrainPipeline::end_tracks() {
  for (const std::uint64_t id : tracker_.take_ended()) {
    if (second_ && second_->id == id) {
      disagree_held_ = disagree_held_ || disagreeing();
      second_->id.reset();
    }
    if (first_ && first_->id == id) first_->id.reset();
    ledger_.forget(id);
    fringes_.erase(id);
  }
}

void GrainPipeline::bind(std::optional<Reference>& reference, const std::optional<Reference>& other) {
  if (!reference || !reference->binding) return;
  for (const Track& t : tracker_.tracks()) {
    if (t.missed != 0 || (other && other->id == t.id) || !contains(t.blob, reference->mark.at)) continue;
    reference->id = t.id;
    reference->binding = false;
    return;
  }
}

void GrainPipeline::follow(std::optional<Reference>& reference) {
  const Track* t = track(reference);
  if (t != nullptr && t->missed == 0 && !t->blob.touches_mask_edge) reference->mark.at = t->blob.centroid;
}

const Track* GrainPipeline::track(const std::optional<Reference>& reference) const {
  if (!reference || !reference->id) return nullptr;
  const std::vector<Track>& tracks = tracker_.tracks();
  const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.id == *reference->id; });
  return it == tracks.end() ? nullptr : &*it;
}

// A reference that cannot be measured is lost whether or not its track lives.
bool GrainPipeline::usable(const Track* reference_track) const {
  return reference_track != nullptr && !reference_track->blob.touches_mask_edge;
}

void GrainPipeline::vote(TimePoint t, bool may_vote) {
  const Track* reference = track(first_);
  if (!may_vote || !usable(reference) || reference->missed != 0) {
    ledger_.drop_pass();
    return;
  }
  const auto measured = fringes_.find(reference->id);
  if (measured == fringes_.end() || measured->second.positions.size() < kMinHistory) {
    ledger_.drop_pass();
    return;
  }
  std::vector<PassLedger::Sample> samples;
  for (const Track& g : tracker_.tracks()) {
    if (g.id == reference->id || g.missed != 0 || g.blob.touches_mask_edge) continue;
    const auto f = fringes_.find(g.id);
    if (f == fringes_.end() || f->second.positions.size() < kMinHistory) continue;
    samples.push_back({g.id, f->second.velocity, f->second.contrast});
  }
  ledger_.update(t, measured->second.velocity, measured->second.contrast, samples);
}

bool GrainPipeline::disagreeing() const {
  if (disagree_held_) return true;
  if (!first_ || !second_ || !second_->id) return false;
  const Tally t = ledger_.tally(*second_->id);
  const bool same_side = first_->mark.side == second_->mark.side;
  const int expected = same_side ? t.same_passes : t.opposite_passes;
  const int contradicting = same_side ? t.opposite_passes : t.same_passes;
  return contradicting >= options_.disagree_passes && contradicting > expected;
}

ReferenceState GrainPipeline::state() const {
  if (!first_) return ReferenceState::None;
  if (!usable(track(first_))) return ReferenceState::Lost;
  if (disagreeing()) return ReferenceState::Disagree;
  return ledger_.pass_running() ? ReferenceState::Reading : ReferenceState::MoveFocus;
}

GrainView GrainPipeline::view_of(const Track& t) const {
  GrainView v;
  v.id = t.id;
  v.outline = ordered_outline(t.blob);
  v.at_edge = t.blob.touches_mask_edge;
  v.reference = (first_ && first_->id == t.id) || (second_ && second_->id == t.id);
  if (const auto f = fringes_.find(t.id); f != fringes_.end()) {
    v.fringe_velocity_px_s = f->second.velocity;
    v.contrast = f->second.contrast;
  }
  if (!first_) return v;

  const Tally tally = ledger_.tally(t.id);
  const bool same_leads = tally.same_weight >= tally.opposite_weight;
  v.score = same_leads ? evidence_score(tally.same_weight, tally.opposite_weight)
                       : evidence_score(tally.opposite_weight, tally.same_weight);
  v.passes_agree = same_leads ? tally.same_passes : tally.opposite_passes;
  v.passes_against = same_leads ? tally.opposite_passes : tally.same_passes;
  v.passes_none = tally.none_passes;
  v.last_vote = ledger_.last_vote(t.id);
  if (!v.reference && !disagreeing()) v.label = decide(tally, first_->mark.side, options_);
  return v;
}

GrainReport GrainPipeline::process(const vision::Frame& frame) {
  GrainReport report;
  report.seq = frame.seq;
  report.timestamp = frame.timestamp;

  const bool blank = frame.width <= 0 || frame.height <= 0 || frame.data.empty();
  const bool resized = previous_.data.empty() ? false
                                              : frame.width != previous_.width || frame.height != previous_.height ||
                                                    frame.pixel_depth != previous_.pixel_depth;
  if (blank || resized) reset();
  if (blank) {
    report.reference = state();
    return report;
  }

  const vision::FrameView view = frame.view();
  const bool too_slow = camera_too_slow(frame.timestamp);
  if (too_slow) report.warning = kTooSlow;

  bool jumped = false;
  if (!previous_.data.empty()) {
    const int search = static_cast<int>(std::ceil(options_.max_shift_px)) + 1;
    const vision::Vec2 shift = estimate_shift(previous_.view(), view, search);
    jumped = std::hypot(shift.x, shift.y) >= options_.max_shift_px;
  }
  previous_ = frame;
  if (jumped) {
    // Nothing from before the jump lines up with what comes after it.
    segmenter_.reset();
    fringes_.clear();
  }

  tracker_.update(segmenter_.update(view));
  end_tracks();
  bind(first_, second_);
  bind(second_, first_);
  measure_fringes(view);
  follow(first_);
  follow(second_);
  vote(frame.timestamp, !too_slow && !jumped);

  report.reference = state();
  report.grains.reserve(tracker_.tracks().size());
  for (const Track& t : tracker_.tracks()) report.grains.push_back(view_of(t));
  return report;
}

Result<std::uint64_t> GrainPipeline::pick(vision::Vec2 at, const std::optional<Reference>& other) const {
  const std::vector<Track>& tracks = tracker_.tracks();
  const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.missed == 0 && contains(t.blob, at); });
  if (it == tracks.end()) return fail(ErrorKind::Config, "no grain at that point");
  if (other && other->id == it->id) return fail(ErrorKind::Config, "that grain is already the other reference");
  if (it->blob.touches_mask_edge) return fail(ErrorKind::Config, "that grain is cut by the frame edge");

  const auto f = fringes_.find(it->id);
  double recent = 0;
  if (f != fringes_.end() && !f->second.contrasts.empty()) {
    const std::vector<double>& c = f->second.contrasts;
    recent = std::accumulate(c.begin(), c.end(), 0.0) / static_cast<double>(c.size());
  }
  if (recent < options_.min_reference_contrast) {
    std::ostringstream why;
    why << "reference contrast " << recent << " is under " << options_.min_reference_contrast;
    return fail(ErrorKind::Config, why.str());
  }
  return it->id;
}

Result<void> GrainPipeline::set_reference(vision::Vec2 frame_px, Side side) {
  const Result<std::uint64_t> id = pick(frame_px, second_);
  if (!id) return fail(id.error());
  if (first_ && first_->mark.side == side) {
    ledger_.drop_pass();
    ledger_.forget(*id);
  } else {
    ledger_.reset();
  }
  disagree_held_ = false;
  first_ = Reference{{frame_px, side}, *id, false};
  return {};
}

Result<void> GrainPipeline::set_second_reference(vision::Vec2 frame_px, Side side) {
  if (!first_) return fail(ErrorKind::Config, "set the first reference before the second");
  const Result<std::uint64_t> id = pick(frame_px, first_);
  if (!id) return fail(id.error());
  disagree_held_ = false;
  second_ = Reference{{frame_px, side}, *id, false};
  return {};
}

void GrainPipeline::clear_reference() {
  first_.reset();
  second_.reset();
  disagree_held_ = false;
  ledger_.reset();
}

void GrainPipeline::reset() {
  segmenter_.reset();
  tracker_.reset();
  ledger_.reset();
  fringes_.clear();
  disagree_held_ = false;
  previous_ = vision::Frame{};
  times_.clear();
  for (std::optional<Reference>* r : {&first_, &second_}) {
    if (*r) {
      (*r)->id.reset();
      (*r)->binding = true;
    }
  }
}

void GrainPipeline::restore_references(ReferenceMark first, std::optional<ReferenceMark> second) {
  ledger_.reset();
  disagree_held_ = false;
  first_ = Reference{first, std::nullopt, true};
  second_.reset();
  if (second) second_ = Reference{*second, std::nullopt, true};
}

std::optional<ReferenceMark> GrainPipeline::reference_mark() const {
  return first_ ? std::optional<ReferenceMark>(first_->mark) : std::nullopt;
}

std::optional<ReferenceMark> GrainPipeline::second_reference_mark() const {
  return second_ ? std::optional<ReferenceMark>(second_->mark) : std::nullopt;
}

}  // namespace pychron::grains
