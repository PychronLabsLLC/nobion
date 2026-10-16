#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/model/types.hpp"

namespace pychron::experiment {

// A named heating schedule, `<lab>/queues/step_heats/<name>.toml`
// (docs/superpowers/specs/2026-10-13-step-heat-template-design.md, 4.2):
//
//   [template]                 # optional
//   name = "laser-12"          # optional; must equal the file stem
//   description = "..."
//   units = "watts"            # defaults for the steps
//   duration = 30
//   cleanup = 15
//
//   [[steps]]                  # one per step, A, B, C, ... in order
//   value = 0.5                # required, > 0
//   duration = 45              # any of the per-step extraction keys below
//
// A key absent from a step comes from [template], then from the form.

struct StepHeatStep {
  double value = 0;
  std::optional<Unit> units;
  std::optional<Duration> duration, cleanup, pre_cleanup, post_cleanup, ramp;
  std::optional<double> beam_diameter, ramp_rate, cryo_temp;
  friend bool operator==(const StepHeatStep&, const StepHeatStep&) = default;
};

struct StepHeatTemplate {
  std::string name, description;
  std::optional<Unit> units;
  std::optional<Duration> duration, cleanup;
  std::vector<StepHeatStep> steps;
  friend bool operator==(const StepHeatTemplate&, const StepHeatTemplate&) = default;
};

// Same limit as the factory's step heat text field.
inline constexpr std::size_t kMaxStepHeatSteps = 1000;

// Non-empty, every character in a-z, 0-9, '_', '-' (a block's name rule).
[[nodiscard]] bool valid_step_heat_name(std::string_view name);

// The value rules of a template: at least one step, at most kMaxStepHeatSteps,
// every value > 0, no negative duration-like field. One line per problem,
// e.g. "step 3 (C): value must be greater than 0", without a file prefix.
// Empty when the template is good.
[[nodiscard]] std::vector<std::string> check_step_heat(const StepHeatTemplate& t);

// Reads a template's text. `name` is the file stem and must pass
// valid_step_heat_name; the result's name is `name`. Every problem is listed
// at once in one Config error, each prefixed "<name>.toml: ".
[[nodiscard]] Result<StepHeatTemplate> parse_step_heat(std::string_view text, std::string_view name);

// Reads a template file: name = stem, messages prefixed with the path.
// Io error when the file cannot be read.
[[nodiscard]] Result<StepHeatTemplate> load_step_heat(const std::filesystem::path& path);

// `t` with each step's units, duration and cleanup cleared where equal to the
// template's own default for that key (a default that is absent clears
// nothing). Reading `to_toml(t)` back gives `normalized(t)`.
[[nodiscard]] StepHeatTemplate normalized(StepHeatTemplate t);

// Canonical text of `normalized(t)`: a [template] table (left out when it
// would be empty), then one [[steps]] table per step, keys in the order of
// the format above, absent optionals and an empty name or description left
// out, numbers written as the queue writer writes them. Writing what this
// text reads back as gives the same bytes. Does not check `t`.
[[nodiscard]] std::string to_toml(const StepHeatTemplate& t);

// The one precedence function (spec 8), used by Add and by the editor's
// greyed cells: `form` with value = s.value; units, duration and cleanup from
// the step, else the template, else the form; pre_cleanup, post_cleanup, ramp,
// beam_diameter, ramp_rate and cryo_temp from the step, else the form.
// Device, position, pattern, script and options are always the form's.
[[nodiscard]] ExtractionSpec resolve_step(const ExtractionSpec& form, const StepHeatTemplate& t,
                                          const StepHeatStep& s);

// One run per step of `t`, steps A, B, ... (past Z: AA, AB, ...), each a copy
// of `tmpl` with extraction = resolve_step(tmpl.extraction, t, step).
[[nodiscard]] std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const StepHeatTemplate& t);

// Extraction time of the whole schedule: sum of estimate_run (measurement 0)
// over make_step_heat of a run whose extraction is `form`.
[[nodiscard]] Duration extraction_time(const ExtractionSpec& form, const StepHeatTemplate& t);

// The step the editor's Add puts after `steps[source]`: a copy of it, with
// value raised by the increment from the step before when `source >= 1`.
// With no steps, StepHeatStep{.value = 1} (`source` ignored). `source` must
// otherwise be a row of `t.steps`.
[[nodiscard]] StepHeatStep step_after(const StepHeatTemplate& t, std::size_t source);

// The editor's Fill from text. With `rows` given, `values` (same count, else a
// Config error "3 values for 2 selected steps") are set on those rows in
// order; with none, each value is appended as step_after the last step with
// that value. A result over kMaxStepHeatSteps is refused. `t` is unchanged on
// any error. Every row must be a row of `t.steps`.
[[nodiscard]] Result<void> fill_values(StepHeatTemplate& t, const std::vector<std::size_t>& rows,
                                       const std::vector<double>& values);

// One line for the step heat panel's preview, e.g.
// "12 runs, steps A to L, 0.5 to 6 W, 21 min extraction": run count, first to
// last step letter, value range in the first run's units, summed extraction
// time (estimate_run, no measurement). Empty for no runs.
[[nodiscard]] std::string step_heat_summary(const std::vector<RunSpec>& runs);

// Templates by name (the file stem).
using StepHeatLibrary = std::map<std::string, StepHeatTemplate, std::less<>>;

struct StepHeatLoad {
  StepHeatLibrary templates;
  std::vector<std::string> problems;  // one line per file that did not load
};

// Every *.toml directly in `dir`, in sorted order. A file that does not load,
// or whose stem fails valid_step_heat_name, is a problem line and is not
// loaded. A missing directory gives no templates and no problem.
[[nodiscard]] StepHeatLoad load_step_heats(const std::filesystem::path& dir);

// The files of <lab>/queues/step_heats, for the editor (the API of
// ConditionalFiles). Every method refuses, with a Config error, a name that
// fails valid_step_heat_name before it touches the disk.
class StepHeatFiles {
 public:
  explicit StepHeatFiles(std::filesystem::path dir) : dir_(std::move(dir)) {}

  // Names without ".toml", sorted; empty when the directory is missing.
  // Files whose stem fails valid_step_heat_name are left out.
  [[nodiscard]] Result<std::vector<std::string>> list() const;
  [[nodiscard]] Result<StepHeatTemplate> read(std::string_view name) const;
  // to_toml(t) to <dir>/<t.name>.toml through a temporary file that is then
  // renamed, so a failed write leaves the old file (or none). Creates the
  // directory. Refuses a template check_step_heat finds a problem in.
  [[nodiscard]] Result<void> write(const StepHeatTemplate& t) const;
  [[nodiscard]] Result<void> remove(std::string_view name) const;
  [[nodiscard]] bool exists(std::string_view name) const;
  [[nodiscard]] std::filesystem::path path(std::string_view name) const;

 private:
  std::filesystem::path dir_;
};

}  // namespace pychron::experiment
