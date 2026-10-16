# Step Heat Templates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A lab keeps named step heat schedules as `<lab>/queues/step_heats/<name>.toml`; the run factory adds one run per step of a picked template; an editor window makes, edits, copies and deletes them.

**Architecture:** Everything that decides something lives in `libs/experiment` (Qt-free): the template types, reader, canonical writer, the one step > template > form precedence function, `make_step_heat` from a template, editing helpers the editor's buttons call, and a files class that only writes inside `step_heats/`. `build_runs` takes the lab's templates. The UI adds a table model and an editor window modelled on `ConditionalsEditorWindow`, and a combo, Edit... and Save as template... on the run factory panel.

**Tech Stack:** C++20, toml++, GoogleTest (`tests/experiment`), Qt 6 Widgets and QtTest (`apps/pychron-ui`, `tests/ui`).

**Spec:** `docs/superpowers/specs/2026-10-13-step-heat-template-design.md` (agreed 2026-10-15). Read it with this plan.

## Global Constraints

- Location: `<lab>/queues/step_heats/<name>.toml`; name = file stem, characters `[a-z0-9_-]` only. Directory optional. Not a queue directory.
- 1 to 1000 steps (`kMaxStepValues`, the text field's limit). `value` required and > 0.
- Step keys, in this order (also the writer's order): `value`, `units`, `duration`, `cleanup`, `pre_cleanup`, `post_cleanup`, `beam_diameter`, `ramp_rate`, `ramp`, `cryo_temp`. `[template]` keys: `name`, `description`, `units`, `duration`, `cleanup`. Same key names and units as a queue row's `[runs.extraction]`; units parsed by `parse_unit` (`model/types.hpp`), as a row's are.
- Precedence: step, then `[template]`, then the form. One function; Add and the editor's greyed cells both call it.
- Template and step heat text never both feed one Add (`Config` error).
- Errors are `pychron::Result<T>` values; parse errors are `ErrorKind::Config`, naming file and step as `step 3 (C): ...`, all problems listed at once joined by `"; "`.
- Writer: canonical TOML, writer then reader gives an equal (normalized) template, writing twice is byte-identical. Floats through the queue writer's float formatting so bytes do not depend on the compiler. No `mark_as_user_file` (pychron reads these back itself).
- The editor writes no file outside `<lab>/queues/step_heats/`.
- Queue file format and `schema_version` unchanged; rows from a template are ordinary rows; `form_from_run` never guesses a template.
- Combo order: "none" first, then templates alphabetically by name. No use counts.
- Out of scope (spec 11): ramps or generators inside a template, applying a template to selected queue rows, legacy `.txt` importer, `elctl` edit commands, use-count ordering. Spec 10 (aliquot per step) is not touched here.
- Docs only: none of the C++ tasks are docs only; each runs `python3 tools/quality_check.py` and `python3 tools/compiler_sweep.py --changed` (with `--build-dir build/dev-ui` for `apps/pychron-ui` or `tests/ui` changes) before its commit.

## Review Focus

1. A template file a lab hand-writes with a negative or non-numeric `duration` / `cleanup` / `ramp`: refused with the step named, not silently made 0 or negative. Test: Task 1, `StepHeatParse.RefusesEachBadFileWithItsMessage` rows for `duration = -5` and `cleanup = "x"`.
2. A step that sets a field to the same value as `[template]`: writer drops it, so a naive round trip is unequal. Rule: reader(writer(t)) == `normalized(t)`. Test: Task 2, `StepHeatWrite.AStepEqualToTheDefaultIsLeftOut`.
3. The panel's combo naming a template that the editor has since deleted or renamed: Add must refuse with "unknown step heat template", not use stale steps. Test: Task 6, `StepHeatBuild.UnknownTemplateIsAnError`; Task 10, `deletedTemplateLeavesCombo`.
4. A file in `step_heats/` whose stem breaks the name rule (`Laser 12.toml`, `.hidden.toml`): a lab problem, never loaded under a made-up name, never writable by the editor. Test: Task 5, `StepHeatFiles.RefusesNamesOutsideTheRule` and `LabTest.ABadStepHeatFileIsALabProblem`.
5. Fill from text with a count that does not match the selected rows: refused with both counts, no partial fill. Test: Task 4, `StepHeatEdit.FillValuesRefusesACountMismatch`.

---

### Task 1: Template types and reader

**Files:**
- Create: `libs/experiment/include/pychron/experiment/factory/step_heat_template.hpp`
- Create: `libs/experiment/src/factory/step_heat_template.cpp`
- `libs/experiment/CMakeLists.txt`: unchanged; it globs `src/*.cpp` recursively
- Test: `tests/experiment/test_step_heat_template.cpp` (new; the directory globs `test_*.cpp`)

**Interfaces:**
- Produces (in `pychron::experiment`):
  - `struct StepHeatStep` and `struct StepHeatTemplate` exactly as spec 5.
  - `inline constexpr std::size_t kMaxStepHeatSteps = 1000;`
  - `bool valid_step_heat_name(std::string_view name);` non-empty, every character in `a-z`, `0-9`, `_`, `-`.
  - `std::vector<std::string> check_step_heat(const StepHeatTemplate& t);` the value rules (no steps, more than 1000, `value <= 0`, negative duration-like field), one line each, `"step 3 (C): value must be greater than 0"` style, without the file prefix. Used by the reader and by the editor's diagnostics.
  - `Result<StepHeatTemplate> parse_step_heat(std::string_view text, std::string_view name);` messages prefixed `"<name>.toml: "`. `name` must pass `valid_step_heat_name`. Result's `name` is `name`.
  - `Result<StepHeatTemplate> load_step_heat(const std::filesystem::path& path);` reads the file (`Io` error if unreadable), name = stem, messages prefixed with `path.string()` instead of `<name>.toml`.

- [x] **Step 1: Write the failing tests**

```cpp
TEST(StepHeatParse, ReadsTheSpecExample) {
  auto t = parse_step_heat(kSpecExample, "laser-12");  // spec 4.2 text verbatim
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(t->name, "laser-12");
  EXPECT_EQ(t->description, "CO2, 12 steps, sanidine");
  EXPECT_EQ(t->units, Unit::Watts);
  EXPECT_EQ(t->duration, Duration(30));
  EXPECT_EQ(t->cleanup, Duration(15));
  ASSERT_EQ(t->steps.size(), 3u);
  EXPECT_EQ(t->steps[0], (StepHeatStep{.value = 0.5}));
  EXPECT_EQ(t->steps[1].duration, Duration(45));
  EXPECT_EQ(t->steps[2].beam_diameter, 2.0);
  EXPECT_EQ(t->steps[2].ramp_rate, 0.1);
}

TEST(StepHeatParse, TemplateTableAndNameAreOptional);  // "[[steps]]\nvalue = 1\n" parses, name = file name

// Table: {text, expected error substring}. Each row also checks ErrorKind::Config.
TEST(StepHeatParse, RefusesEachBadFileWithItsMessage);
//   "[[steps]]\nvalue = 0\n"                         -> "laser-12.toml: step 1 (A): value must be greater than 0"
//   "[[steps]]\nvalue = 1\n[[steps]]\nvalue = -1\n"  -> "step 2 (B): value must be greater than 0"
//   "[[steps]]\nduration = 5\n"                       -> "step 1 (A): value is required"
//   "[[steps]]\nvalue = 1\ncolour = 2\n"             -> "step 1 (A): unknown key 'colour'"
//   "[[steps]]\nvalue = 1\nunits = \"kelvin\"\n"     -> "step 1 (A): unknown units 'kelvin'"
//   "[[steps]]\nvalue = 1\nduration = -5\n"          -> "step 1 (A): 'duration' must not be negative"
//   "[[steps]]\nvalue = 1\ncleanup = \"x\"\n"        -> "step 1 (A): 'cleanup' must be a number"
//   "[template]\nunits = \"kelvin\"\n[[steps]]\nvalue = 1\n" -> "template: unknown units 'kelvin'"
//   "[template]\ndevice = \"co2\"\n[[steps]]\nvalue = 1\n"   -> "template: unknown key 'device'"
//   "[template]\nname = \"other\"\n[[steps]]\nvalue = 1\n"   -> "template.name 'other' does not match the file name 'laser-12'"
//   "[template]\n"                                   -> "laser-12.toml: no [[steps]]"
//   "[[runs]]\nidentifier = \"a\"\n"                 -> "root: unknown key 'runs'"
//   "[[steps]\n"                                     -> "laser-12.toml: syntax error"
//   1001 steps                                       -> "more than 1000 steps"

TEST(StepHeatParse, ListsEveryProblemAtOnce);  // step 1 value 0 and step 3 unknown key: both substrings in one error
TEST(StepHeatParse, NamesFollowTheBlockRule);  // valid: "laser-12", "a_b", "x9"; invalid: "", "Laser", "a b", "a.b", "../x", ".x"
TEST(StepHeatLoad, NamesTheFileAndTakesTheStem);  // temp dir file "co2-6.toml": name "co2-6"; bad file: error starts with its path
```

- [x] **Step 2: Run tests to verify they fail**

Run: `cmake --build --preset dev --target pychron_experiment_tests` — Expected: compile error, `step_heat_template.hpp` not found.

- [x] **Step 3: Implement the header and reader**

Read with toml++ directly in `step_heat_template.cpp` (queue_toml.cpp's `Reader` is in an anonymous namespace and writes non-optional fields; do not move it). A local `Errors` collector in the same `"<file>: <where>: <msg>"` and `"; "` form as `queue_toml.cpp`. Integers accepted wherever a number is (`node.value<double>()`). After reading, append `check_step_heat`'s lines (each prefixed by the file). Step label from `step_name(i)` (`factory/increments.hpp`): `"step " + std::to_string(i + 1) + " (" + step_name(i) + ")"`.

- [x] **Step 4: Run tests to verify they pass**

Run: `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='StepHeat*'` — Expected: all PASS.

- [x] **Step 5: Checks and commit**

Run `python3 tools/quality_check.py` (exit 0) and `python3 tools/compiler_sweep.py --changed` (no warnings).

```bash
git add libs/experiment/include/pychron/experiment/factory/step_heat_template.hpp libs/experiment/src/factory/step_heat_template.cpp libs/experiment/CMakeLists.txt tests/experiment/test_step_heat_template.cpp
git commit -m "feat(experiment): step heat template type and reader"
```

### Task 2: Canonical writer

**Files:**
- Create: `libs/experiment/src/model/toml_text.hpp` and `toml_text.cpp` (private to the library, under `src/`)
- Modify: `libs/experiment/src/model/queue_file.cpp` (its `fmt(const std::string&)` and `fmt(double)` move to `toml_text`; behaviour unchanged)
- Modify: `libs/experiment/src/factory/step_heat_template.cpp`, its header
- Modify: `libs/experiment/CMakeLists.txt`
- Test: `tests/experiment/test_step_heat_template.cpp`; existing `tests/experiment/test_queue_file.cpp` must pass unchanged

**Interfaces:**
- Produces:
  - `namespace pychron::experiment::detail { std::string toml_string(std::string_view s); std::string toml_float(double v); }` — the bodies of `queue_file.cpp`'s `fmt` overloads, moved verbatim.
  - `StepHeatTemplate normalized(StepHeatTemplate t);` clears a step's `units`, `duration`, `cleanup` where equal to the template's default.
  - `std::string to_toml(const StepHeatTemplate& t);`
- Consumes: Task 1's `parse_step_heat`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(StepHeatWrite, RoundTripsTheSpecExample) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t);
  auto back = parse_step_heat(to_toml(*t), "laser-12");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, *t);
}
TEST(StepHeatWrite, WritingTwiceIsByteIdentical);  // to_toml(parse(to_toml(t))) == to_toml(t)
TEST(StepHeatWrite, AStepEqualToTheDefaultIsLeftOut);
//   t.duration = 30s; step A duration = 30s; step B duration = 45s
//   text has no "duration = 30.0" under [[steps]] A, has "duration = 45.0" under B;
//   parse(to_toml(t)) == normalized(t); normalized(t).steps[0].duration == std::nullopt
TEST(StepHeatWrite, KeysInSpecOrder);
//   a step with every key set: to_toml text has value, units, duration, cleanup, pre_cleanup,
//   post_cleanup, beam_diameter, ramp_rate, ramp, cryo_temp in that order (find() positions increasing);
//   [template] has name, description, units, duration, cleanup in that order
TEST(StepHeatWrite, FloatsAreWrittenAsTheQueueWriterWritesThem);  // value 0.1 -> "value = 0.1"; 30 -> "duration = 30.0"
```

- [ ] **Step 2: Run to verify they fail** (`to_toml` / `normalized` undeclared).

- [ ] **Step 3: Move the scalar formatters, then write `to_toml`**

`queue_file.cpp` calls `detail::toml_string` / `detail::toml_float` from its `fmt` overloads (or replaces them). Output layout:

```toml
[template]
name = "laser-12"
description = "..."
units = "watts"
duration = 30.0
cleanup = 15.0

[[steps]]
value = 0.5
```

Empty `description` and absent optionals omitted; `name` written when non-empty; units written with `to_string(Unit)`. Write `normalized(t)`, not `t`.

- [ ] **Step 4: Run** `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='StepHeat*:QueueFile*'` — Expected: PASS, queue file tests unchanged.

- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(experiment): canonical step heat template writer"
```

### Task 3: Precedence and make_step_heat from a template

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/factory/step_heat_template.hpp`, `src/factory/step_heat_template.cpp`
- Modify: `libs/experiment/include/pychron/experiment/factory/increments.hpp`, `src/factory/increments.cpp` (old `make_step_heat(tmpl, values)` rewritten over the new one)
- Modify: `libs/experiment/include/pychron/experiment/factory/form.hpp`, `src/factory/form.cpp` (`template_from_values`, `extraction_of`; text path in `build_runs` goes through `template_from_values`)
- Test: `tests/experiment/test_step_heat_template.cpp`; `tests/experiment/test_run_factory.cpp` and `test_factory_form.cpp` pass unchanged

**Interfaces:**
- Produces:
  - `ExtractionSpec resolve_step(const ExtractionSpec& form, const StepHeatTemplate& t, const StepHeatStep& s);` returns `form` with `value` = `s.value`; `units`, `duration`, `cleanup` from step, else template, else form; `pre_cleanup`, `post_cleanup`, `ramp`, `beam_diameter`, `ramp_rate`, `cryo_temp` from step, else form. Device, position, pattern, script, options always the form's. The one precedence function (spec 8).
  - `std::vector<RunSpec> make_step_heat(const RunSpec& tmpl, const StepHeatTemplate& t);` one run per step, `id.step = step_name(i)`, `extraction = resolve_step(tmpl.extraction, t, steps[i])`.
  - In `form.hpp`: `StepHeatTemplate template_from_values(const std::vector<double>& values, const FactoryForm& form);` `[template]` `units`, `duration`, `cleanup` = the form's; steps carry `value` only; name empty.
  - In `form.hpp`: `ExtractionSpec extraction_of(const FactoryForm& form);` value, units, duration, cleanup of the form, the rest default (what the editor greys against).
  - `Duration extraction_time(const ExtractionSpec& form, const StepHeatTemplate& t);` sum of `estimate_run` (measurement `Duration(0)`) over `make_step_heat` of a run with `extraction = form`.
- Consumes: Task 1 types.

- [ ] **Step 1: Write the failing tests**

```cpp
// One table, every field, each of the three sources (spec 8: "tested once with a table").
TEST(StepHeatPrecedence, StepThenTemplateThenForm);
//   form: units Watts, duration 10, cleanup 5, pre_cleanup 1, beam_diameter 1.0, device "co2", script "x"
//   template: units Temp, duration 30 (no cleanup)
//   step A {value 2}:                 units Temp,    duration 30, cleanup 5, beam 1.0, device "co2", script "x"
//   step B {value 3, duration 45, cleanup 7, units Percent, beam 2.0, ramp 4s, ramp_rate 0.1, cryo 20, pre 2, post 3}:
//                                     each of those from the step
TEST(StepHeatMake, GivesLettersValuesAndPerStepDurations);
//   template from spec example, run with identifier "20001": 3 runs, steps "A","B","C",
//   values 0.5,1.0,1.5, durations 30,45,30, run 3 beam 2.0, identifiers all "20001"
TEST(StepHeatMake, TwentySevenStepsGoPastZ);  // step 27 is "AA"
TEST(StepHeatMake, TextPathGivesTodaysRuns);
//   for values {5,10,15}: make_step_heat(tmpl, values) == make_step_heat(tmpl, template_from_values(values, form))
//   where form duration/cleanup/units match tmpl
TEST(StepHeatMake, ExtractionTimeSumsTheSteps);
//   form pre_cleanup 0; template duration 30 cleanup 15; steps {1},{2, duration 60}: 45 + 75 = 120 s
```

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement**; old `make_step_heat(tmpl, values)` builds a `StepHeatTemplate` whose steps carry only `value` and calls the new one; `build_runs` text branch calls `make_step_heat(*tmpl, template_from_values(*values, form))`.

- [ ] **Step 4: Run** `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='StepHeat*:RunFactory*:FactoryForm*'` — Expected: PASS.

- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(experiment): make a step heat from a template, step over template over form"
```

### Task 4: Editing helpers and the preview summary

Qt-free so the editor's buttons and the panel's preview are tested with GoogleTest.

**Files:**
- Modify: `step_heat_template.hpp` / `.cpp`
- Test: `tests/experiment/test_step_heat_template.cpp`

**Interfaces:**
- Produces:
  - `StepHeatStep step_after(const StepHeatTemplate& t, std::size_t source);` copy of `steps[source]`; when `source >= 1`, `value += steps[source].value - steps[source - 1].value`. `t.steps` empty: `StepHeatStep{.value = 1}` (and `source` ignored).
  - `Result<void> fill_values(StepHeatTemplate& t, const std::vector<std::size_t>& rows, const std::vector<double>& values);` rows non-empty: `values.size()` must equal `rows.size()`, else `Config` error `"3 values for 2 selected steps"`, `t` unchanged; values set in row order. Rows empty: each value appended as `step_after(t, last)` with that value. Result over 1000 steps: `Config` error, `t` unchanged.
  - `std::string step_heat_summary(const std::vector<RunSpec>& runs);` `"12 runs, steps A to L, 0.5 to 6 W, 21 min extraction"`. Steps: first and last distinct step letters by `step_index`; values: min to max of `extraction.value` written with `%g`, unit of the first run (`W`, `°C`, `%`, `A`, `V`); one value: `"5 W"`. Time: `estimate_run(r, 0s)` summed; under 60 s `"45 s"`, under 1 h whole minutes rounded `"21 min"`, else `"1 h 5 min"`. Empty runs: `""`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(StepHeatEdit, StepAfterAddsTheLastIncrement);   // steps 1,2.5: step_after(t,1).value == 4.0; other fields copied
TEST(StepHeatEdit, StepAfterOneStepCopiesIt);        // steps {3, duration 45}: value 3, duration 45
TEST(StepHeatEdit, StepAfterNothingIsOneWatt);       // empty: value 1, no optionals
TEST(StepHeatEdit, FillValuesSetsSelectedRows);      // rows {0,2}, values {7,9}: steps[0]=7, steps[2]=9, steps[1] unchanged
TEST(StepHeatEdit, FillValuesAppendsWithoutSelection);  // 2 steps, values {5,6}: 4 steps, last two values 5,6
TEST(StepHeatEdit, FillValuesRefusesACountMismatch); // error "3 values for 2 selected steps"; t == before
TEST(StepHeatSummary, SaysCountStepsRangeAndTime);
//   12 runs A..L, values 0.5..6 W, each 105 s -> "12 runs, steps A to L, 0.5 to 6 W, 21 min extraction"
TEST(StepHeatSummary, OneRunAndLongSchedules);      // 1 run 5 W 45 s -> "1 run, step A, 5 W, 45 s extraction"; 3900 s -> "1 h 5 min"
```

- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `--gtest_filter='StepHeat*'` — PASS.
- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(experiment): step heat editing helpers and summary"
```

### Task 5: Files, lab loading, `LabPaths`

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/lab/lab.hpp` (layout comment line `<lab>/queues/step_heats/*.toml  step heat templates for the run factory (optional)`; `LabPaths` member functions; `Lab::step_heats`)
- Modify: `libs/experiment/src/lab/lab.cpp`
- Modify: `step_heat_template.hpp` / `.cpp` (files class and directory loader)
- Test: `tests/experiment/test_step_heat_template.cpp`, `tests/experiment/test_lab.cpp`

**Interfaces:**
- Produces:
  - In `LabPaths`: `std::filesystem::path queues() const { return dir / "queues"; }` and `std::filesystem::path step_heats() const { return queues() / "step_heats"; }`. Member functions, so every `LabPaths{dir, line, spectrometer}` aggregate initialisation stays valid. If the queue-directory work (`2026-10-13-queue-directory-design.md`) has landed first with its own `queues`, use it and add only `step_heats`.
  - `using StepHeatLibrary = std::map<std::string, StepHeatTemplate, std::less<>>;`
  - `struct StepHeatLoad { StepHeatLibrary templates; std::vector<std::string> problems; };`
  - `StepHeatLoad load_step_heats(const std::filesystem::path& dir);` every `*.toml` directly in `dir`, sorted; a bad file or a stem failing `valid_step_heat_name` is a problem line (`"<path>: name 'Laser 12' must be lower-case letters, digits, '_' or '-'"`), not loaded. Missing directory: empty, no problem.
  - `class StepHeatFiles` with the API of `ConditionalFiles` (`conditionals/library.hpp`): `explicit StepHeatFiles(std::filesystem::path dir)`; `Result<std::vector<std::string>> list() const` (sorted); `Result<StepHeatTemplate> read(std::string_view name) const`; `Result<void> write(const StepHeatTemplate& t) const` (`to_toml`, temp file then rename, creates the directory; name from `t.name`); `Result<void> remove(std::string_view name) const`; `bool exists(std::string_view name) const`; `std::filesystem::path path(std::string_view name) const`. Every method refuses (`Config`) a name failing `valid_step_heat_name` before touching the disk.
  - `Lab::step_heats` (`StepHeatLibrary`), filled by `load_lab` from `paths.step_heats()`, its problems appended to `Lab::problems`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(StepHeatFiles, WriteReadListRemove);        // temp dir: write "b" then "a"; list == {"a","b"}; read("a") == normalized(a); remove; list == {"b"}
TEST(StepHeatFiles, WriteCreatesTheDirectory);
TEST(StepHeatFiles, RefusesNamesOutsideTheRule); // "../x", "a/b", "Laser", "" -> errors; nothing created outside dir (check parent listing unchanged)
TEST(StepHeatLoadDir, MissingDirectoryIsEmpty);
TEST(StepHeatLoadDir, BadFilesAreProblemsGoodOnesLoad);  // good.toml, bad.toml (value 0), "Bad Name.toml": 1 template, 2 problems
TEST_F(LabTest, StepHeatsLoadFromQueuesStepHeats);      // copy of example lab + queues/step_heats/x.toml: lab.step_heats has "x"
TEST_F(LabTest, ABadStepHeatFileIsALabProblem);         // problems has one line naming the file; lab still loads
TEST_F(LabTest, AStepHeatsDirectoryIsNotAQueueDirectory); // a valid queue text saved as queues/step_heats/q.toml is a problem (not a template), never a queue
```

- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement**; `load_lab` calls `load_step_heats(paths.step_heats())` after blocks.
- [ ] **Step 4: Run** `--gtest_filter='StepHeat*:LabTest.*'` — PASS.
- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(experiment): load step heat templates from queues/step_heats"
```

### Task 6: `build_runs` and `next_form` take a template

**Files:**
- Modify: `libs/experiment/include/pychron/experiment/factory/form.hpp`, `src/factory/form.cpp`
- Test: `tests/experiment/test_factory_form.cpp` (existing cases unchanged)

**Interfaces:**
- Produces:
  - `FactoryForm` gains `std::string step_heat_template;` ("" = none), after `step_heat`,, part of `operator==`.
  - `Result<std::vector<RunSpec>> build_runs(const FactoryForm& form, const IdentifierRules& ids, const StepHeatLibrary& step_heats = {});`
  - `Result<FactoryForm> next_form(const FactoryForm& form, const IncrementOptions& inc, const IdentifierRules& ids, const StepHeatLibrary& step_heats = {});` keeps `step_heat_template`, as it keeps the text.
  - `form_from_run` leaves `step_heat_template` empty.
- Rules, in this order inside `build_runs`: template named and `trim(step_heat)` non-empty → `Config` `"choose a step heat template or type step values, not both"`; template named but not in `step_heats` → `"unknown step heat template '<name>'"`; template on a type that does not heat → the text's existing message. Template with `one_run_per_hole` and a position of more than one hole: `expand_positions` (with `identifier_step` when non-zero and not special, as today) then `make_step_heat` of each hole's run, concatenated in hole order. Otherwise `make_step_heat(*tmpl, t)` with the position unsplit.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(StepHeatBuild, TemplateMakesOneRunPerStep);           // "laser-12" 3 steps: 3 runs A..C, durations from the template
TEST(StepHeatBuild, TemplateAndTextIsAnError);             // message above
TEST(StepHeatBuild, UnknownTemplateIsAnError);             // "unknown step heat template 'nope'"
TEST(StepHeatBuild, TemplateNeedsATypeThatHeats);          // identifier "bu": error contains "does not"
TEST(StepHeatBuild, PerHoleGivesWholeScheduleForEachHole);
//   position "1-3", one_run_per_hole, 2-step template: 6 runs; holes 1,1,2,2,3,3; steps A,B,A,B,A,B
TEST(StepHeatBuild, PerHoleWithIdentifierStep);           // identifier_step 1, "20001": identifiers 20001,20001,20002,20002,...
TEST(StepHeatBuild, WithoutPerHoleOneScheduleOverThePosition); // 2 runs, each position "1-3"
TEST(StepHeatBuild, NextFormKeepsTheTemplate);
TEST(StepHeatBuild, FormFromRunNeverNamesATemplate);
```

- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `--gtest_filter='StepHeat*:FactoryForm*:RunFactory*'` — PASS. Then `ctest --preset dev-quick` (form.hpp reaches `elctl` and the UI).
- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(experiment): run factory adds a step heat from a named template"
```

### Task 7: Example lab template

Read `libs/setup/AGENTS.md` first.

**Files:**
- Create: `configs/examples/queues/step_heats/laser-12.toml` (12 steps, 0.5 to 6 W by 0.5, `[template]` units watts, duration 30, cleanup 15, description "CO2, 12 steps, sanidine"; written by `to_toml` so it is canonical)
- Create: `profiles/instrument-common/queues/step_heats/laser-12.toml` (same text)
- Modify: `profiles/instrument-common/profile.toml` (a `[[files]]` entry `template = "queues/step_heats/laser-12.toml"`, `to = "queues/step_heats/laser-12.toml"`, after the block's entry)
- Test: `tests/experiment/test_lab.cpp`, `tests/integration/test_install_e2e.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
// test_lab.cpp, in TheExampleLabLoadsAndItsQueueChecks:
EXPECT_TRUE(lab.step_heats.contains("laser-12"));
EXPECT_EQ(lab.step_heats.at("laser-12").steps.size(), 12u);
// test_lab.cpp, new:
TEST_F(LabTest, TheExampleTemplateIsCanonical);  // file bytes == to_toml(load_step_heat(path))
// test_install_e2e.cpp fixture SetUp, after the problems check:
ASSERT_TRUE(lab_.step_heats.contains("laser-12"));
```

- [ ] **Step 2: Run** `build/dev/tests/experiment/pychron_experiment_tests --gtest_filter='LabTest.*'` — FAIL (no file).
- [ ] **Step 3: Add the files and the profile entry.**
- [ ] **Step 4: Run** `LabTest.*`, `pychron_setup_tests` (filtered to the profile suites), and `cmake --build --preset dev --target pychron_install_e2e_tests && build/dev/tests/integration/pychron_install_e2e_tests` — PASS.
- [ ] **Step 5: Commit**

```bash
git commit -m "feat(setup): ship an example step heat template"
```

### Task 8: Editor table model

**Files:**
- Create: `apps/pychron-ui/src/step_heat_table_model.hpp`, `.cpp`
- Modify: `apps/pychron-ui/CMakeLists.txt` (source and header lists, beside `conditional_table_model`)
- Test: `tests/ui/test_step_heat_editor.cpp` (new; QtTest, globbed as `pychron_ui_test_step_heat_editor`)

**Interfaces:**
- Produces: `class StepHeatTableModel : public QAbstractTableModel` in `pychron::ui`.
  - `enum Column { Step, Value, Units, Duration, Cleanup, BeamDiameter, RampRate, ColumnCount };` headers "Step", "Value", "Units", "Duration (s)", "Cleanup (s)", "Beam diameter", "Ramp rate".
  - `void set_template(experiment::StepHeatTemplate t);` `const experiment::StepHeatTemplate& tmpl() const;` `[template]` fields are edited through `void set_template_defaults(std::optional<Unit>, std::optional<Duration> duration, std::optional<Duration> cleanup, std::string description)`.
  - `void set_form_extraction(experiment::ExtractionSpec form);` what inherited cells grey against.
  - Row operations, each one undoable by `revert` only (no undo stack): `int add_step(std::optional<int> selected)` (inserts `step_after(t, selected or last)` after that row, returns its row); `int insert_step(int row)` (copy of `row` before it); `void remove_rows(std::vector<int> rows)`; `bool move_up(int row)`, `bool move_down(int row)`; `void fill(Column c, const QVariant& v, std::vector<int> rows)` (empty rows: every row); `Result<void> fill_values(std::vector<int> rows, const QString& text)` (`parse_step_values` then `experiment::fill_values`).
  - `data`: Step column = `step_name(row)`, read-only. Empty optional: `DisplayRole` = resolved value from `resolve_step(form, t, step)`, `ForegroundRole` = palette's disabled text colour, `EditRole` = empty. `setData` with empty text clears the optional.
  - `signal void edited();` after any change.

- [ ] **Step 1: Write the failing QtTest slots**

```cpp
void stepColumnIsLettersAndReadOnly();
void emptyCellShowsTheInheritedValueGreyed();   // template duration 30, step none -> "30", foreground == disabled; EditRole empty
void clearingACellInherits();                    // setData(Duration, "") -> step duration nullopt
void addCopiesSelectedOrLastWithIncrement();     // values 1,2 -> add(nullopt) gives 3; add(0) inserts after row 0 with value 1 (no previous)
void insertMoveAndDelete();                      // insert(1), move_up(2), move_down(0), remove_rows({0,2}); values and letters checked
void fillSetsOneColumnOnSelectedRows();          // fill(Cleanup, 20, {0,2}); row 1 unchanged; rows empty -> all rows
void fillValuesFromText();                       // "5:2.5:4" on no selection appends 4 steps 5,7.5,10,12.5; "1,2" on 3 rows -> error text
```

- [ ] **Step 2: Run** `cmake --build --preset dev-ui --target pychron_ui_test_step_heat_editor` — FAIL (no header).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `QT_QPA_PLATFORM=offscreen build/dev-ui/tests/ui/pychron_ui_test_step_heat_editor` — PASS.
- [ ] **Step 5: Checks** (`quality_check.py`; `compiler_sweep.py --changed --build-dir build/dev-ui`) **and commit**

```bash
git commit -m "feat(ui): step heat table model"
```

### Task 9: Editor window

**Files:**
- Create: `apps/pychron-ui/src/step_heat_editor_window.hpp`, `.cpp`
- Create: `apps/pychron-ui/src/toml_comments.hpp` (`bool has_toml_comment(const std::string& text)`, moved from `conditionals_editor_window.cpp`'s `has_comment`, which then calls it)
- Modify: `apps/pychron-ui/src/conditionals_editor_window.cpp`, `apps/pychron-ui/CMakeLists.txt`
- Test: `tests/ui/test_step_heat_editor.cpp`; `tests/ui/test_conditionals_editor.cpp` passes unchanged

**Interfaces:**
- Consumes: `StepHeatFiles`, `StepHeatTableModel`, `check_step_heat`, `extraction_time`, `normalized`.
- Produces: `class StepHeatEditorWindow : public QMainWindow` in `pychron::ui`, API shaped as `ConditionalsEditorWindow`'s:
  - `StepHeatEditorWindow(const experiment::lab::Lab& lab, std::unique_ptr<QSettings> settings = nullptr, QWidget* parent = nullptr);` files from `StepHeatFiles(lab.paths.step_heats())`.
  - `enum class Unsaved { Save, Discard, Cancel };`
  - `QStringList file_names() const;` `bool open(const QString& name);` `bool new_file(const QString& name, QString* error = nullptr);` `bool new_from(experiment::StepHeatTemplate t, const QString& name, QString* error = nullptr);` (an unsaved file holding `t`, for Save as template) `bool duplicate(const QString& name, const QString& as, QString* error = nullptr);` `bool delete_file(const QString& name, QString* error = nullptr);` (asks first) `bool save(QString* error = nullptr);` `bool save_as(const QString& name, QString* error = nullptr);` (rename = save as; old file kept) `void revert();` `bool modified() const;` `QString current_name() const;` `StepHeatTableModel& model();` `QStringList diagnostic_lines() const;` `QString totals_text() const;` (`"12 steps, 21 min extraction"`, through `extraction_time`) `void set_form_extraction(std::function<experiment::ExtractionSpec()> f);` `void set_from_factory(std::function<std::optional<experiment::StepHeatTemplate>()> f);` (Edit > New from Run Factory Text; disabled when null or it answers nullopt).
  - Dialog hooks for tests: `set_ask_unsaved`, `set_confirm`, `set_ask_name(std::function<std::optional<QString>()>)`.
  - Signals: `void saved(const QString& name);` `void filesChanged();`
  - Diagnostics are `check_step_heat`'s lines; non-empty disables Save and Save As. A file whose text has comments asks before the first save (`has_toml_comment`). Close and switching file ask about unsaved changes.

- [ ] **Step 1: Write the failing QtTest slots** (lab: example lab copied to a `QTemporaryDir`, as `test_conditionals_editor.cpp` does)

```cpp
void listsTheLabTemplatesSorted();
void newSaveWritesOnlyUnderStepHeats();   // new "x", add 2 steps, save: <lab>/queues/step_heats/x.toml exists; no other file in <lab> changed (directory listing before/after)
void aBadValueBlocksSave();               // set value 0 -> diagnostic_lines() has "step 1 (A): value must be greater than 0"; save() false; file unchanged
void revertRestoresTheFile();
void saveAsCopiesAndOpensTheCopy();
void duplicateAndDeleteAskFirst();        // confirm hook answers false: file kept; true: removed, filesChanged emitted
void unsavedChangesAskedOnSwitch();       // Cancel keeps the current file open
void commentsAskedBeforeSave();           // file with "# note": confirm hook called once
void totalsSayCountAndTime();             // laser-12: "12 steps, ..."
void newFromFactoryText();                // set_from_factory returns template_from_values({5,10}) -> unsaved file with 2 steps
```

- [ ] **Step 2: Run** — FAIL (no header).
- [ ] **Step 3: Implement**: left list with +, duplicate, − buttons; right top name (read-only label), description, units combo (with "(form)"), duration and cleanup boxes (with an empty "(form)" state); table; row buttons Add, Insert, Delete, Up, Down, Fill..., Fill from text...; totals label; diagnostics list; File menu Save, Save As..., Revert; Edit menu New from Run Factory Text.
- [ ] **Step 4: Run** `pychron_ui_test_step_heat_editor` and `pychron_ui_test_conditionals_editor` — PASS.
- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(ui): step heat template editor window"
```

### Task 10: Run factory panel and experiment window

**Files:**
- Modify: `apps/pychron-ui/src/run_factory_panel.hpp`, `.cpp`
- Modify: `apps/pychron-ui/src/experiment_window.hpp`, `.cpp`
- Test: `tests/ui/test_run_factory_panel.cpp`, `tests/ui/test_experiment_window.cpp`

**Interfaces:**
- Consumes: `build_runs(form, ids, step_heats)`, `next_form(..., step_heats)`, `load_step_heats`, `template_from_values`, `step_heat_summary`, `StepHeatEditorWindow`.
- Produces on `RunFactoryPanel`:
  - Combo `step_heat_template` (object name), items "none" then `step_heats_` names in map order (alphabetical); `QPushButton` `edit_step_heat` ("Edit..."); `QPushButton` `save_step_heat` ("Save as template..."), enabled when the text parses to at least one value.
  - Member `experiment::StepHeatLibrary step_heats_`, initialised from `lab_.step_heats`; `void refresh_step_heats();` reloads it with `load_step_heats(lab_.paths.step_heats())`, keeps the picked name when still there, else picks "none".
  - Picking a template clears and disables the text; typing text sets the combo to "none". Both disabled for a type that does not heat (beside `step_heat_` in `refresh`).
  - `form()` / `set_form()` carry `step_heat_template`. `build_runs` and `next_form` get `step_heats_`.
  - Preview for a step heat (template or text) is `step_heat_summary(runs)`; other cases unchanged.
  - Signals: `void editStepHeatRequested(const QString& name);` `void saveStepHeatRequested(const experiment::StepHeatTemplate& t);` (from `template_from_values(values, form())`).
  - For tests: `QStringList step_heat_choices() const;` `void choose_step_heat(const QString& name);`
- On `ExperimentWindow`: `StepHeatEditorWindow* open_step_heat_editor(const QString& name = {});` built like `open_conditionals_editor` (own `QSettings` on the same store); menu `Menu::Scripts` "Step &Heat Templates..." beside "Conditionals Editor..."; `saved` and `filesChanged` call `factory_->refresh_step_heats()`; panel's `editStepHeatRequested` opens it on the name; `saveStepHeatRequested` opens it and calls `new_from` with a name from its name dialog; `set_form_extraction` and `set_from_factory` hooks wired to the panel's form. The menu action and Edit... stay enabled while a queue runs (the editor edits files only; spec 7); `set_locked` does not touch them.

- [ ] **Step 1: Write the failing QtTest slots**

```cpp
// test_run_factory_panel.cpp (lab with templates "b-two" and "a-one" written to queues/step_heats before the panel is made)
void stepHeatCombosListsAlphabetically();   // {"none","a-one","b-two"}
void pickingATemplateDisablesTheText();     // field_enabled("step_heat") false, text empty; typing text -> combo "none"
void previewSaysCountAndTime();             // preview_text() == step_heat_summary(expected runs)
void addInsertsOneRowPerStep();             // model rows += steps.size(), steps A.. in order
void deletedTemplateLeavesCombo();          // remove file, refresh_step_heats(): choices lack it, combo on "none"
void saveAsTemplateEmitsTheTextAsATemplate(); // text "5,10": signal arg == template_from_values({5,10}, form())
void typeThatDoesNotHeatDisablesTheRow();   // identifier "bu": combo, edit, save disabled
void editStaysEnabledWhenLocked();          // set_locked(true): field_enabled("edit_step_heat") true
// test_experiment_window.cpp
void stepHeatEditorOpensFromMenuAndRefreshesFactory(); // save a new template in the editor -> factory choices include it
```

- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `pychron_ui_test_run_factory_panel`, `pychron_ui_test_experiment_window`, `pychron_ui_test_step_heat_editor` — PASS.
- [ ] **Step 5: Checks and commit**

```bash
git commit -m "feat(ui): step heat templates in the run factory"
```

### Task 11: User docs and full run

**Files:**
- Modify: `docs/user/04-experiments.md` (run factory section: the template combo, the editor, file location and format with the spec 4.2 example; precedence step > template > form; Add only, no apply to existing rows)
- Modify: `docs/superpowers/specs/2026-10-13-step-heat-template-design.md` only if code departed from it (Status line: built)

- [ ] **Step 1: Write the docs section.**
- [ ] **Step 2: Full build and tests**: `cmake --build --preset dev-ui` then `ctest --preset dev-ui` (covers `dev` and `tests/ui`) — all pass. With persistence configured, `pychron_install_e2e_tests` is in that run.
- [ ] **Step 3: Whole-tree checks**: `python3 tools/quality_check.py` (exit 0), `python3 tools/compiler_sweep.py --build-dir build/dev-ui` (no warnings).
- [ ] **Step 4: Commit**

```bash
git commit -m "docs(experiment): step heat templates in the user guide"
```

## Open questions

None.
