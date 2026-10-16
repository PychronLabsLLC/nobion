#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
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

}  // namespace pychron::experiment
