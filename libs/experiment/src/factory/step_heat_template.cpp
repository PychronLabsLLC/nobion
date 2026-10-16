#include "pychron/experiment/factory/step_heat_template.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <utility>

#include <toml++/toml.hpp>

#include "model/toml_text.hpp"
#include "pychron/experiment/factory/increments.hpp"

namespace pychron::experiment {
namespace {

constexpr std::array<std::string_view, 2> kRootKeys = {"template", "steps"};
constexpr std::array<std::string_view, 5> kTemplateKeys = {"name", "description", "units", "duration", "cleanup"};
constexpr std::array<std::string_view, 10> kStepKeys = {
    "value", "units", "duration", "cleanup", "pre_cleanup", "post_cleanup", "beam_diameter", "ramp_rate", "ramp",
    "cryo_temp"};

std::string step_label(std::size_t i) {
  return "step " + std::to_string(i + 1) + " (" + step_name(static_cast<int>(i)) + ")";
}

// Every problem of one file, each "<file>: <where>: <msg>", joined by "; " (as queue_toml.cpp).
class Errors {
 public:
  explicit Errors(std::string file) : file_(std::move(file)) {}
  void add(const std::string& where, const std::string& msg) { lines_.push_back(file_ + ": " + where + ": " + msg); }
  void add(const std::string& msg) { lines_.push_back(file_ + ": " + msg); }
  [[nodiscard]] bool any() const { return !lines_.empty(); }
  [[nodiscard]] std::string text() const {
    std::string out;
    for (const auto& line : lines_) {
      if (!out.empty()) out += "; ";
      out += line;
    }
    return out;
  }

 private:
  std::string file_;
  std::vector<std::string> lines_;
};

void check_keys(const toml::table& t, std::span<const std::string_view> allowed, const std::string& where,
                Errors& err) {
  const std::set<std::string_view> ok(allowed.begin(), allowed.end());
  for (const auto& [k, v] : t)
    if (!ok.contains(k.str())) err.add(where, "unknown key '" + std::string(k.str()) + "'");
}

void read_number(const toml::table& t, std::string_view key, std::optional<double>& out, const std::string& where,
                 Errors& err) {
  const auto* node = t.get(key);
  if (!node) return;
  if (auto v = node->value<double>(); v && !node->is_boolean()) out = *v;
  else err.add(where, "'" + std::string(key) + "' must be a number");
}

void read_duration(const toml::table& t, std::string_view key, std::optional<Duration>& out, const std::string& where,
                   Errors& err) {
  std::optional<double> seconds;
  read_number(t, key, seconds, where, err);
  if (seconds) out = Duration(*seconds);
}

void read_units(const toml::table& t, std::optional<Unit>& out, const std::string& where, Errors& err) {
  const auto* node = t.get("units");
  if (!node) return;
  const auto text = node->value<std::string>();
  if (!text) {
    err.add(where, "'units' must be a string");
    return;
  }
  if (auto u = parse_unit(*text)) out = *u;
  else err.add(where, "unknown units '" + *text + "'");
}

void read_string(const toml::table& t, std::string_view key, std::string& out, const std::string& where, Errors& err) {
  const auto* node = t.get(key);
  if (!node) return;
  if (auto v = node->value<std::string>()) out = *v;
  else err.add(where, "'" + std::string(key) + "' must be a string");
}

// Reads one [[steps]] table; false when `value` is missing or not a number
// (already reported, so the value rule is not reported again).
bool read_step(const toml::table& t, StepHeatStep& s, const std::string& where, Errors& err) {
  check_keys(t, kStepKeys, where, err);
  std::optional<double> value;
  if (t.contains("value")) read_number(t, "value", value, where, err);
  else err.add(where, "value is required");
  if (value) s.value = *value;
  read_units(t, s.units, where, err);
  read_duration(t, "duration", s.duration, where, err);
  read_duration(t, "cleanup", s.cleanup, where, err);
  read_duration(t, "pre_cleanup", s.pre_cleanup, where, err);
  read_duration(t, "post_cleanup", s.post_cleanup, where, err);
  read_number(t, "beam_diameter", s.beam_diameter, where, err);
  read_number(t, "ramp_rate", s.ramp_rate, where, err);
  read_duration(t, "ramp", s.ramp, where, err);
  read_number(t, "cryo_temp", s.cryo_temp, where, err);
  return value.has_value();
}

void check_duration(const std::optional<Duration>& d, std::string_view key, const std::string& where,
                    std::vector<std::string>& out) {
  if (!d) return;
  const double s = d->count();
  if (!std::isfinite(s)) out.push_back(where + ": '" + std::string(key) + "' must be a finite number");
  else if (s < 0) out.push_back(where + ": '" + std::string(key) + "' must not be negative");
}

std::string value_rule(std::size_t i) { return step_label(i) + ": value must be greater than 0"; }

// `file` is the prefix of every message: "<name>.toml" or the file's path.
Result<StepHeatTemplate> parse(std::string_view text, std::string_view name, const std::string& file) {
  if (!valid_step_heat_name(name))
    return fail(ErrorKind::Config, file + ": '" + std::string(name) +
                                       "' is not a valid step heat template name (a-z, 0-9, '_', '-')");
  auto parsed = toml::parse(text, file);
  if (!parsed)
    return fail(ErrorKind::Config, file + ": syntax error: " + std::string(parsed.error().description()));
  const toml::table& root = parsed.table();

  Errors err(file);
  StepHeatTemplate t;
  t.name = std::string(name);

  check_keys(root, kRootKeys, "root", err);
  if (const auto* node = root.get("template")) {
    if (const auto* tt = node->as_table()) {
      check_keys(*tt, kTemplateKeys, "template", err);
      std::string given;
      read_string(*tt, "name", given, "template", err);
      if (tt->contains("name") && given != name)
        err.add("template.name '" + given + "' does not match the file name '" + std::string(name) + "'");
      read_string(*tt, "description", t.description, "template", err);
      read_units(*tt, t.units, "template", err);
      read_duration(*tt, "duration", t.duration, "template", err);
      read_duration(*tt, "cleanup", t.cleanup, "template", err);
    } else {
      err.add("root", "'template' must be a table");
    }
  }

  std::set<std::string> skip;  // a step with no value is reported once, as "value is required"
  if (const auto* node = root.get("steps")) {
    const auto* steps = node->as_array();
    if (steps && steps->is_array_of_tables()) {
      t.steps.reserve(steps->size());
      for (const auto& item : *steps) {
        const std::size_t i = t.steps.size();
        StepHeatStep s;
        if (!read_step(*item.as_table(), s, step_label(i), err)) skip.insert(value_rule(i));
        t.steps.push_back(s);
      }
    } else {
      err.add("root", "'steps' must be an array of tables ([[steps]])");
    }
  }

  for (const auto& line : check_step_heat(t))
    if (!skip.contains(line)) err.add(line);
  if (err.any()) return fail(ErrorKind::Config, err.text());
  return t;
}

}  // namespace

bool valid_step_heat_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

std::vector<std::string> check_step_heat(const StepHeatTemplate& t) {
  std::vector<std::string> out;
  check_duration(t.duration, "duration", "template", out);
  check_duration(t.cleanup, "cleanup", "template", out);
  if (t.steps.empty()) out.emplace_back("no [[steps]]");
  if (t.steps.size() > kMaxStepHeatSteps) out.push_back("more than " + std::to_string(kMaxStepHeatSteps) + " steps");
  for (std::size_t i = 0; i < t.steps.size(); ++i) {
    const auto& s = t.steps[i];
    const std::string where = step_label(i);
    if (!std::isfinite(s.value)) out.push_back(where + ": value must be a finite number");
    else if (s.value <= 0) out.push_back(value_rule(i));
    check_duration(s.duration, "duration", where, out);
    check_duration(s.cleanup, "cleanup", where, out);
    check_duration(s.pre_cleanup, "pre_cleanup", where, out);
    check_duration(s.post_cleanup, "post_cleanup", where, out);
    check_duration(s.ramp, "ramp", where, out);
  }
  return out;
}

Result<StepHeatTemplate> parse_step_heat(std::string_view text, std::string_view name) {
  return parse(text, name, std::string(name) + ".toml");
}

Result<StepHeatTemplate> load_step_heat(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, path.string() + ": cannot be read");
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) return fail(ErrorKind::Io, path.string() + ": cannot be read");
  return parse(text, path.stem().string(), path.string());
}

StepHeatTemplate normalized(StepHeatTemplate t) {
  for (auto& s : t.steps) {
    if (t.units && s.units == t.units) s.units.reset();
    if (t.duration && s.duration == t.duration) s.duration.reset();
    if (t.cleanup && s.cleanup == t.cleanup) s.cleanup.reset();
  }
  return t;
}

std::string to_toml(const StepHeatTemplate& t) {
  const StepHeatTemplate n = normalized(t);
  std::string out;
  const auto header = [&out](std::string_view h) {
    if (!out.empty()) out += '\n';
    out += h;
    out += '\n';
  };
  const auto raw = [&out](std::string_view key, const std::string& value) {
    out += key;
    out += " = ";
    out += value;
    out += '\n';
  };
  const auto str = [&raw](std::string_view key, const std::string& v) {
    if (!v.empty()) raw(key, detail::toml_string(v));
  };
  const auto units = [&raw](const std::optional<Unit>& u) {
    if (u) raw("units", detail::toml_string(to_string(*u)));
  };
  const auto num = [&raw](std::string_view key, const std::optional<double>& v) {
    if (v) raw(key, detail::toml_float(*v));
  };
  const auto dur = [&raw](std::string_view key, const std::optional<Duration>& d) {
    if (d) raw(key, detail::toml_float(d->count()));
  };

  if (!n.name.empty() || !n.description.empty() || n.units || n.duration || n.cleanup) {
    header("[template]");
    str("name", n.name);
    str("description", n.description);
    units(n.units);
    dur("duration", n.duration);
    dur("cleanup", n.cleanup);
  }
  for (const auto& s : n.steps) {
    header("[[steps]]");
    raw("value", detail::toml_float(s.value));
    units(s.units);
    dur("duration", s.duration);
    dur("cleanup", s.cleanup);
    dur("pre_cleanup", s.pre_cleanup);
    dur("post_cleanup", s.post_cleanup);
    num("beam_diameter", s.beam_diameter);
    num("ramp_rate", s.ramp_rate);
    dur("ramp", s.ramp);
    num("cryo_temp", s.cryo_temp);
  }
  return out;
}

ExtractionSpec resolve_step(const ExtractionSpec& form, const StepHeatTemplate& t, const StepHeatStep& s) {
  ExtractionSpec e = form;
  e.value = s.value;
  e.units = s.units.value_or(t.units.value_or(form.units));
  e.duration = s.duration.value_or(t.duration.value_or(form.duration));
  e.cleanup = s.cleanup.value_or(t.cleanup.value_or(form.cleanup));
  e.pre_cleanup = s.pre_cleanup.value_or(form.pre_cleanup);
  e.post_cleanup = s.post_cleanup.value_or(form.post_cleanup);
  e.ramp = s.ramp.value_or(form.ramp);
  if (s.beam_diameter) e.beam_diameter = s.beam_diameter;
  if (s.ramp_rate) e.ramp_rate = s.ramp_rate;
  if (s.cryo_temp) e.cryo_temp = s.cryo_temp;
  return e;
}

std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const StepHeatTemplate& t) {
  std::vector<RunSpec> out;
  out.reserve(t.steps.size());
  for (std::size_t i = 0; i < t.steps.size(); ++i) {
    out.push_back(tmpl);
    out.back().id.step = step_name(static_cast<int>(i));
    out.back().extraction = resolve_step(tmpl.extraction, t, t.steps[i]);
  }
  return out;
}

Duration extraction_time(const ExtractionSpec& form, const StepHeatTemplate& t) {
  RunSpec run;
  run.extraction = form;
  Duration total{0};
  for (const auto& r : make_step_heat(run, t)) total += estimate_run(r, Duration(0));
  return total;
}

}  // namespace pychron::experiment
