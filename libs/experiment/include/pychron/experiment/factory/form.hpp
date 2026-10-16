#pragma once

// The run factory form (experiment-window design, run factory panel): what an
// operator fills in to add runs, and the rules that turn it into RunSpecs.
// Qt-free so the panel stays a thin view over it.
//
//   build_runs        the form -> one run, one run per hole of a multi-hole
//                     position, or a step heat (one run per value, steps A, B ...)
//   form_from_run     a run (a queue row) -> the form
//   with_lab_defaults the lab's defaults.toml entry for (type, device) applied
//   next_form         pychron's auto-increment after Add: identifier and/or
//                     position advanced past what was just added

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/factory/defaults.hpp"
#include "pychron/experiment/factory/increments.hpp"
#include "pychron/experiment/factory/step_heat_template.hpp"
#include "pychron/experiment/model/identifiers.hpp"
#include "pychron/experiment/model/rules.hpp"
#include "pychron/experiment/model/run_spec.hpp"

namespace pychron::experiment {

struct FactoryForm {
  std::string identifier;  // a special identifier ("bu", "a") makes a special run
  std::optional<int> aliquot;
  std::string step;
  std::string extract_device;
  std::string position;           // "", "4", "1-4", "1,3,5-7"
  bool one_run_per_hole = true;   // a multi-hole position becomes one run per hole
  int identifier_step = 0;        // with one run per hole: identifier advance per run
  double value = 0;
  Unit units = Unit::Watts;
  double duration_s = 0, cleanup_s = 0;
  std::string step_heat;          // "" or step values: "5, 10, 15" or "5:2.5:4" (start:increment:count)
  std::string step_heat_template; // "" or the name of a template in the lab's step_heats
  std::string script, plan, post_equilibration, post_measurement, comment;
  ParamOverrides overrides;       // carried from defaults or a row; not edited in the form
  std::vector<std::string> conditionals;  // the lab's conditionals files every run gets
  friend bool operator==(const FactoryForm&, const FactoryForm&) = default;
};

// "5, 10, 15" (any of , ; whitespace as separators) or "start:increment:count".
// Empty text is an empty list. Config error for anything else, a count below 1
// or more than 1000 values.
Result<std::vector<double>> parse_step_values(std::string_view text);

// Which fields the form's analysis type uses (rules_for of the classified type).
FieldRules form_rules(const FactoryForm& form, const IdentifierRules& ids);

// The runs Add inserts, in order. Fields the type does not use are stripped
// (strip_for_type). Config error for an invalid identifier, step, position or
// step-heat text, or a step heat on a type without heating.
//
// A named `step_heat_template` is looked up in `step_heats`: Config error when
// the step heat text is also filled in, or when the name is not there. With
// one run per hole and a multi-hole position, each hole's run (identifier
// stepped as without a template) gets the whole schedule, holes in order;
// otherwise one schedule over the unsplit position.
Result<std::vector<RunSpec>> build_runs(const FactoryForm& form, const IdentifierRules& ids,
                                        const StepHeatLibrary& step_heats = {});

// Never names a template: a queue row does not say which one made it.

FactoryForm form_from_run(const RunSpec& run);

// The step heat text field's values as a template: [template] units, duration
// and cleanup are the form's, steps carry a value only, name empty.
[[nodiscard]] StepHeatTemplate template_from_values(const std::vector<double>& values, const FactoryForm& form);

// The form's value, units, duration and cleanup as an extraction, every other
// field default: what the template editor greys its cells against.
[[nodiscard]] ExtractionSpec extraction_of(const FactoryForm& form);

// The defaults for the form's (type, device) applied over its plan, scripts,
// overrides and the extraction fields the entry sets. nullopt when the table
// has no entry (exact device or "*").
std::optional<FactoryForm> with_lab_defaults(const FactoryForm& form, const IdentifierRules& ids,
                                             const DefaultsTable& defaults);

// The form after Add. The identifier advances by `inc.identifier` past the
// last identifier added (specials never advance; a changed identifier drops a
// fixed aliquot and step); the position advances by `inc.position` past the
// last hole added (a range keeps its width). Step-heat text, the named
// template and the other fields are kept. `step_heats` as for build_runs.
Result<FactoryForm> next_form(const FactoryForm& form, const IncrementOptions& inc, const IdentifierRules& ids,
                              const StepHeatLibrary& step_heats = {});

}  // namespace pychron::experiment
