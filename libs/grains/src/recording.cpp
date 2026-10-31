#include "pychron/grains/recording.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <system_error>

#include "pychron/core/user_file.hpp"
#include "pychron/grains/score.hpp"

namespace pychron::grains {
namespace {

using json = nlohmann::json;

constexpr double kTolerance = 1e-9;

const char* side_text(Side s) { return s == Side::Below ? "below" : "above"; }

const char* kind_text(RecordEvent::Kind k) {
  switch (k) {
    case RecordEvent::Kind::Set: return "set";
    case RecordEvent::Kind::SetSecond: return "set_second";
    case RecordEvent::Kind::Clear: return "clear";
    case RecordEvent::Kind::Restore: return "restore";
  }
  return "clear";
}

const char* state_text(ReferenceState s) {
  switch (s) {
    case ReferenceState::None: return "none";
    case ReferenceState::Lost: return "lost";
    case ReferenceState::MoveFocus: return "move_focus";
    case ReferenceState::Reading: return "reading";
    case ReferenceState::Disagree: return "disagree";
  }
  return "none";
}

const char* vote_text(Vote v) {
  switch (v) {
    case Vote::Same: return "same";
    case Vote::Opposite: return "opposite";
    case Vote::None: return "none";
  }
  return "none";
}

json mark_json(const ReferenceMark& m) { return json{{"x", m.at.x}, {"y", m.at.y}, {"side", side_text(m.side)}}; }

ReferenceMark mark_from(const json& j) {
  ReferenceMark m;
  m.at = {j.at("x").get<double>(), j.at("y").get<double>()};
  const auto side = j.at("side").get<std::string>();
  if (side != "below" && side != "above") throw std::runtime_error("unknown side '" + side + "'");
  m.side = side == "below" ? Side::Below : Side::Above;
  return m;
}

json options_json(const GrainOptions& o) {
  return json{{"min_contrast", o.min_contrast},
              {"min_reference_contrast", o.min_reference_contrast},
              {"min_passes", o.min_passes},
              {"min_score", o.min_score},
              {"track_gap_frames", o.track_gap_frames},
              {"max_shift_px", o.max_shift_px},
              {"disagree_passes", o.disagree_passes},
              {"min_fps", o.min_fps},
              {"segment_window", o.segment_window},
              {"ring_px", o.ring_px},
              {"min_area_px", o.min_area_px},
              {"min_pass_frames", o.min_pass_frames},
              {"min_ref_speed_px_s", o.min_ref_speed_px_s}};
}

GrainOptions options_from(const json& j) {
  GrainOptions o;
  o.min_contrast = j.at("min_contrast").get<double>();
  o.min_reference_contrast = j.at("min_reference_contrast").get<double>();
  o.min_passes = j.at("min_passes").get<int>();
  o.min_score = j.at("min_score").get<double>();
  o.track_gap_frames = j.at("track_gap_frames").get<int>();
  o.max_shift_px = j.at("max_shift_px").get<double>();
  o.disagree_passes = j.at("disagree_passes").get<int>();
  o.min_fps = j.at("min_fps").get<double>();
  o.segment_window = j.at("segment_window").get<int>();
  o.ring_px = j.at("ring_px").get<int>();
  o.min_area_px = j.at("min_area_px").get<int>();
  o.min_pass_frames = j.at("min_pass_frames").get<int>();
  o.min_ref_speed_px_s = j.at("min_ref_speed_px_s").get<double>();
  return o;
}

json grain_json(const GrainView& g) {
  json outline = json::array();
  for (const vision::Vec2& p : g.outline) outline.push_back(json::array({p.x, p.y}));
  return json{{"id", g.id},
              {"outline", std::move(outline)},
              {"v", g.fringe_velocity_px_s},
              {"contrast", g.contrast},
              {"vote", vote_text(g.last_vote)},
              {"label", std::string(label_text(g.label))},
              {"score", g.score},
              {"agree", g.passes_agree},
              {"against", g.passes_against},
              {"none", g.passes_none},
              {"reference", g.reference},
              {"at_edge", g.at_edge}};
}

// What of a report is compared. `seq` is left out of the comparison: the
// recorder numbers frames its own way.
json report_json(const GrainReport& r) {
  json grains = json::array();
  for (const GrainView& g : r.grains) grains.push_back(grain_json(g));
  return json{{"state", state_text(r.reference)}, {"warning", r.warning}, {"grains", std::move(grains)}};
}

// Numbers within kTolerance, everything else exact. A non-finite value is
// written as null, and so compares equal to another.
bool same_json(const json& a, const json& b) {
  if (a.is_number() && b.is_number()) return std::abs(a.get<double>() - b.get<double>()) <= kTolerance;
  if (a.type() != b.type()) return false;
  if (a.is_object()) {
    if (a.size() != b.size()) return false;
    return std::ranges::all_of(a.items(), [&](const auto& kv) { return b.contains(kv.key()) && same_json(kv.value(), b.at(kv.key())); });
  }
  if (a.is_array()) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (!same_json(a[i], b[i])) return false;
    return true;
  }
  return a == b;
}

json event_json(const RecordEvent& e) {
  json j{{"kind", kind_text(e.kind)}};
  if (e.kind != RecordEvent::Kind::Clear) j["first"] = mark_json(e.first);
  if (e.second) j["second"] = mark_json(*e.second);
  return j;
}

RecordEvent event_from(const json& j) {
  RecordEvent e;
  const auto kind = j.at("kind").get<std::string>();
  if (kind == "set") e.kind = RecordEvent::Kind::Set;
  else if (kind == "set_second") e.kind = RecordEvent::Kind::SetSecond;
  else if (kind == "clear") e.kind = RecordEvent::Kind::Clear;
  else if (kind == "restore") e.kind = RecordEvent::Kind::Restore;
  else throw std::runtime_error("unknown event '" + kind + "'");
  if (e.kind != RecordEvent::Kind::Clear) e.first = mark_from(j.at("first"));
  if (j.contains("second")) e.second = mark_from(j.at("second"));
  return e;
}

Unexpected<Error> corrupt(const std::filesystem::path& file, std::size_t line, const std::string& what) {
  return fail(ErrorKind::Config, file.string() + ":" + std::to_string(line) + ": " + what);
}

struct Recorded {
  std::int64_t t_ns = 0;
  std::vector<RecordEvent> events;
  json expected;  // state, warning, grains
};

Result<Recorded> parse_frame(const json& j) {
  Recorded r;
  try {
    r.t_ns = j.at("t_ns").get<std::int64_t>();
    for (const json& e : j.at("events")) r.events.push_back(event_from(e));
    r.expected = json{{"state", j.at("state")}, {"warning", j.at("warning")}, {"grains", j.at("grains")}};
    if (!r.expected["grains"].is_array()) return fail(ErrorKind::Config, "grains is not a list");
  } catch (const std::exception& e) {
    return fail(ErrorKind::Config, e.what());
  }
  return r;
}

}  // namespace

GrainRecording::GrainRecording(std::filesystem::path dir, std::ofstream out)
    : dir_(std::move(dir)), recorder_(dir_, vision::Provenance::Raw, vision::FinderMode::Hole, 1.0), out_(std::move(out)) {}

Result<GrainRecording> GrainRecording::open(const std::filesystem::path& dir, const GrainOptions& options) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + dir.string() + ": " + ec.message());
  const auto file = dir / "tracks.jsonl";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return fail(ErrorKind::Io, "cannot open " + file.string() + " for writing");
  out << json{{"options", options_json(options)}}.dump() << '\n';
  out.flush();
  if (!out) return fail(ErrorKind::Io, "write failed for " + file.string());
  return GrainRecording(dir, std::move(out));
}

Result<void> GrainRecording::add(const vision::Frame& frame, const GrainReport& report, std::span<const RecordEvent> events) {
  if (finished_) return fail(ErrorKind::Config, "recording is finished");
  if (auto r = recorder_.add(frame.view()); !r) return r;
  json evs = json::array();
  for (const RecordEvent& e : events) evs.push_back(event_json(e));
  json line = report_json(report);
  line["t_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(frame.timestamp.time_since_epoch()).count();
  line["seq"] = report.seq;
  line["events"] = std::move(evs);
  out_ << line.dump() << '\n';
  out_.flush();
  if (!out_) return fail(ErrorKind::Io, "write failed for " + (dir_ / "tracks.jsonl").string());
  ++frames_;
  return {};
}

Result<void> GrainRecording::finish() {
  if (finished_) return {};
  finished_ = true;
  out_.close();
  if (out_.fail()) return fail(ErrorKind::Io, "write failed for " + (dir_ / "tracks.jsonl").string());
  if (frames_ > 0) {
    if (auto r = recorder_.finish(); !r) return r;
  } else {
    // FrameRecorder refuses a case with no frame; an empty recording is still
    // a folder replay can open.
    vision::FixtureCase empty;
    empty.dir = dir_;
    empty.provenance = vision::Provenance::Raw;
    empty.mode = vision::FinderMode::Hole;
    empty.expected_radius_px = 1.0;
    if (auto r = vision::save_case(empty); !r) return r;
  }
  (void)mark_as_user_file(dir_ / "tracks.jsonl");
  (void)mark_as_user_file(dir_ / "case.toml");
  return {};
}

Result<ReplaySummary> replay_folder(const std::filesystem::path& dir) {
  const auto file = dir / "tracks.jsonl";
  std::ifstream in(file, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + file.string());

  std::string text;
  if (!std::getline(in, text)) return corrupt(file, 1, "empty file");
  const json head = json::parse(text, nullptr, false);
  if (head.is_discarded() || !head.is_object() || !head.contains("options")) return corrupt(file, 1, "first line is not the options");
  GrainOptions options;
  try {
    options = options_from(head.at("options"));
  } catch (const std::exception& e) {
    return corrupt(file, 1, std::string("options: ") + e.what());
  }

  std::vector<Recorded> recorded;
  while (std::getline(in, text)) {
    const std::size_t number = recorded.size() + 2;
    const json line = json::parse(text, nullptr, false);
    if (line.is_discarded() || !line.is_object()) return corrupt(file, number, "not a JSON object");
    auto frame = parse_frame(line);
    if (!frame) return corrupt(file, number, frame.error().what);
    recorded.push_back(std::move(*frame));
  }

  ReplaySummary summary;
  if (recorded.empty()) return summary;

  auto fixture = vision::load_case(dir);
  if (!fixture) return fail(fixture.error());
  if (fixture->frames.size() != recorded.size())
    return fail(ErrorKind::Config, file.string() + ": " + std::to_string(recorded.size()) + " frames recorded, " + std::to_string(fixture->frames.size()) +
                                       " in case.toml");

  std::int64_t now_ns = 0;
  vision::RecordedSource source(std::move(*fixture), [&now_ns] { return TimePoint{} + std::chrono::nanoseconds(now_ns); });
  GrainPipeline pipeline(options);
  GrainReport last;
  for (std::size_t i = 0; i < recorded.size(); ++i) {
    for (const RecordEvent& e : recorded[i].events) {
      Result<void> done;
      switch (e.kind) {
        case RecordEvent::Kind::Set: done = pipeline.set_reference(e.first.at, e.first.side); break;
        case RecordEvent::Kind::SetSecond: done = pipeline.set_second_reference(e.first.at, e.first.side); break;
        case RecordEvent::Kind::Clear: pipeline.clear_reference(); break;
        case RecordEvent::Kind::Restore: pipeline.restore_references(e.first, e.second); break;
      }
      if (!done) return fail(ErrorKind::Config, "frame " + std::to_string(i + 1) + ": " + kind_text(e.kind) + " event refused: " + done.error().what);
    }
    now_ns = recorded[i].t_ns;
    auto frame = source.grab();
    if (!frame) return fail(frame.error());
    last = pipeline.process(*frame);
    if (!same_json(report_json(last), recorded[i].expected)) ++summary.mismatches;
    ++summary.frames;
  }
  summary.final_grains = std::move(last.grains);
  return summary;
}

std::string summary_text(const ReplaySummary& summary) {
  std::ostringstream os;
  os << "frames " << summary.frames << ", mismatches " << summary.mismatches << ", grains " << summary.final_grains.size() << '\n';
  os << std::fixed << std::setprecision(2);
  for (const GrainView& g : summary.final_grains) {
    const std::string_view label = label_text(g.label);
    os << "grain " << g.id << ": " << (label.empty() ? "no label" : label) << ", v " << g.fringe_velocity_px_s << " px/s, contrast " << g.contrast
       << ", score " << g.score << ", passes " << g.passes_agree << " agree " << g.passes_against << " against " << g.passes_none << " none";
    if (g.reference) os << ", reference";
    if (g.at_edge) os << ", at edge";
    os << '\n';
  }
  return os.str();
}

}  // namespace pychron::grains
