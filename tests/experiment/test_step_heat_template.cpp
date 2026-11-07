#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "nobion/experiment/factory/form.hpp"
#include "nobion/experiment/factory/increments.hpp"
#include "nobion/experiment/factory/step_heat_template.hpp"
#include "temp_path.hpp"

using namespace nobion;
using namespace nobion::experiment;

namespace {

// Spec 4.2, verbatim.
constexpr const char* kSpecExample = R"toml([template]
name = "laser-12"            # optional; must equal the file stem when given
description = "CO2, 12 steps, sanidine"
units = "watts"              # default for steps: watts | temp | percent
duration = 30                # s, default for steps
cleanup = 15                 # s, default for steps

[[steps]]
value = 0.5

[[steps]]
value = 1.0
duration = 45

[[steps]]
value = 1.5
beam_diameter = 2.0
ramp_rate = 0.1
)toml";

std::string many_steps(int n) {
  std::string text;
  for (int i = 0; i < n; ++i) text += "[[steps]]\nvalue = 1\n";
  return text;
}

}  // namespace

TEST(StepHeatParse, ReadsTheSpecExample) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(t->name, "laser-12");
  EXPECT_EQ(t->description, "CO2, 12 steps, sanidine");
  EXPECT_EQ(t->units, Unit::Watts);
  EXPECT_EQ(t->duration, Duration(30));
  EXPECT_EQ(t->cleanup, Duration(15));
  ASSERT_EQ(t->steps.size(), 3u);
  StepHeatStep first;
  first.value = 0.5;
  EXPECT_EQ(t->steps[0], first);
  EXPECT_EQ(t->steps[1].value, 1.0);
  EXPECT_EQ(t->steps[1].duration, Duration(45));
  EXPECT_FALSE(t->steps[1].cleanup.has_value());
  EXPECT_EQ(t->steps[2].beam_diameter, 2.0);
  EXPECT_EQ(t->steps[2].ramp_rate, 0.1);
}

TEST(StepHeatParse, ReadsEveryStepKey) {
  auto t = parse_step_heat(
      "[[steps]]\nvalue = 800\nunits = \"temp\"\nduration = 1\ncleanup = 2\npre_cleanup = 3\npost_cleanup = 4\n"
      "beam_diameter = 5\nramp_rate = 6\nramp = 7\ncryo_temp = 8\n",
      "furnace");
  ASSERT_TRUE(t) << t.error().what;
  ASSERT_EQ(t->steps.size(), 1u);
  const auto& s = t->steps[0];
  EXPECT_EQ(s.value, 800.0);
  EXPECT_EQ(s.units, Unit::Temp);
  EXPECT_EQ(s.duration, Duration(1));
  EXPECT_EQ(s.cleanup, Duration(2));
  EXPECT_EQ(s.pre_cleanup, Duration(3));
  EXPECT_EQ(s.post_cleanup, Duration(4));
  EXPECT_EQ(s.beam_diameter, 5.0);
  EXPECT_EQ(s.ramp_rate, 6.0);
  EXPECT_EQ(s.ramp, Duration(7));
  EXPECT_EQ(s.cryo_temp, 8.0);
}

TEST(StepHeatParse, TemplateTableAndNameAreOptional) {
  auto t = parse_step_heat("[[steps]]\nvalue = 1\n", "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(t->name, "laser-12");
  EXPECT_TRUE(t->description.empty());
  EXPECT_FALSE(t->units.has_value());
  EXPECT_FALSE(t->duration.has_value());
  EXPECT_FALSE(t->cleanup.has_value());
  ASSERT_EQ(t->steps.size(), 1u);
  EXPECT_EQ(t->steps[0].value, 1.0);

  auto named = parse_step_heat("[template]\nname = \"laser-12\"\n[[steps]]\nvalue = 1\n", "laser-12");
  ASSERT_TRUE(named) << named.error().what;
  EXPECT_EQ(named->name, "laser-12");
}

TEST(StepHeatParse, RefusesEachBadFileWithItsMessage) {
  struct Case {
    std::string text, expected;
  };
  const std::vector<Case> cases = {
      {"[[steps]]\nvalue = 0\n", "laser-12.toml: step 1 (A): value must be greater than 0"},
      {"[[steps]]\nvalue = 1\n[[steps]]\nvalue = -1\n", "step 2 (B): value must be greater than 0"},
      {"[[steps]]\nduration = 5\n", "step 1 (A): value is required"},
      {"[[steps]]\nvalue = 1\ncolour = 2\n", "step 1 (A): unknown key 'colour'"},
      {"[[steps]]\nvalue = 1\nunits = \"kelvin\"\n", "step 1 (A): unknown units 'kelvin'"},
      {"[[steps]]\nvalue = 1\nduration = -5\n", "step 1 (A): 'duration' must not be negative"},
      {"[[steps]]\nvalue = 1\ncleanup = \"x\"\n", "step 1 (A): 'cleanup' must be a number"},
      {"[template]\nunits = \"kelvin\"\n[[steps]]\nvalue = 1\n", "template: unknown units 'kelvin'"},
      {"[template]\ndevice = \"co2\"\n[[steps]]\nvalue = 1\n", "template: unknown key 'device'"},
      {"[template]\nname = \"other\"\n[[steps]]\nvalue = 1\n",
       "template.name 'other' does not match the file name 'laser-12'"},
      {"[template]\n", "laser-12.toml: no [[steps]]"},
      {"[[runs]]\nidentifier = \"a\"\n", "root: unknown key 'runs'"},
      {"[[steps]\n", "laser-12.toml: syntax error"},
      {many_steps(1001), "more than 1000 steps"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.text.substr(0, 80));
    auto t = parse_step_heat(c.text, "laser-12");
    ASSERT_FALSE(t);
    EXPECT_EQ(t.error().kind, ErrorKind::Config);
    EXPECT_NE(t.error().what.find(c.expected), std::string::npos) << t.error().what;
  }
}

TEST(StepHeatParse, AMissingValueIsReportedOnce) {
  auto t = parse_step_heat("[[steps]]\nduration = 5\n", "laser-12");
  ASSERT_FALSE(t);
  EXPECT_EQ(t.error().what.find("greater than 0"), std::string::npos) << t.error().what;
}

TEST(StepHeatParse, AcceptsExactly1000Steps) {
  auto t = parse_step_heat(many_steps(1000), "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(t->steps.size(), kMaxStepHeatSteps);
}

TEST(StepHeatParse, ListsEveryProblemAtOnce) {
  auto t = parse_step_heat("[[steps]]\nvalue = 0\n[[steps]]\nvalue = 1\n[[steps]]\nvalue = 2\nhue = 3\n", "laser-12");
  ASSERT_FALSE(t);
  EXPECT_NE(t.error().what.find("step 1 (A): value must be greater than 0"), std::string::npos) << t.error().what;
  EXPECT_NE(t.error().what.find("step 3 (C): unknown key 'hue'"), std::string::npos) << t.error().what;
  EXPECT_NE(t.error().what.find("; "), std::string::npos) << t.error().what;
}

TEST(StepHeatParse, RefusesANameOutsideTheRule) {
  auto t = parse_step_heat("[[steps]]\nvalue = 1\n", "Laser 12");
  ASSERT_FALSE(t);
  EXPECT_EQ(t.error().kind, ErrorKind::Config);
}

TEST(StepHeatParse, NamesFollowTheBlockRule) {
  for (const char* good : {"laser-12", "a_b", "x9"}) EXPECT_TRUE(valid_step_heat_name(good)) << good;
  for (const char* bad : {"", "Laser", "a b", "a.b", "../x", ".x"}) EXPECT_FALSE(valid_step_heat_name(bad)) << bad;
}

TEST(StepHeatCheck, ListsValueRulesWithoutAFilePrefix) {
  StepHeatTemplate t;
  EXPECT_EQ(check_step_heat(t), std::vector<std::string>{"no [[steps]]"});

  t.steps.resize(3);
  t.steps[0].value = 1;
  t.steps[1].value = 0;
  t.steps[2].value = 2;
  t.steps[2].ramp = Duration(-1);
  t.cleanup = Duration(-2);
  const auto lines = check_step_heat(t);
  const std::vector<std::string> expected = {"template: 'cleanup' must not be negative",
                                             "step 2 (B): value must be greater than 0",
                                             "step 3 (C): 'ramp' must not be negative"};
  EXPECT_EQ(lines, expected);

  t.cleanup.reset();
  t.steps[1].value = 0.1;
  t.steps[2].ramp.reset();
  EXPECT_TRUE(check_step_heat(t).empty());
}

TEST(StepHeatLoad, NamesTheFileAndTakesTheStem) {
  std::random_device rd;
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pychron-step-heat-" + std::to_string(rd()) + std::to_string(rd()));
  std::filesystem::create_directories(dir);

  const auto good = dir / "co2-6.toml";
  std::ofstream(good) << "[[steps]]\nvalue = 1\n[[steps]]\nvalue = 2\n";
  auto t = load_step_heat(good);
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(t->name, "co2-6");
  EXPECT_EQ(t->steps.size(), 2u);

  const auto bad = dir / "bad.toml";
  std::ofstream(bad) << "[[steps]]\nvalue = 0\n";
  auto b = load_step_heat(bad);
  ASSERT_FALSE(b);
  EXPECT_EQ(b.error().kind, ErrorKind::Config);
  EXPECT_EQ(b.error().what.rfind(bad.string() + ": ", 0), 0u) << b.error().what;

  auto missing = load_step_heat(dir / "absent.toml");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Io);

  std::filesystem::remove_all(dir);
}

namespace {

// Position of `needle` in `text` at or after `from`; fails the test when absent.
std::size_t at(const std::string& text, const std::string& needle, std::size_t from = 0) {
  const auto pos = text.find(needle, from);
  EXPECT_NE(pos, std::string::npos) << "'" << needle << "' not in:\n" << text;
  return pos;
}

StepHeatStep every_key_step() {
  StepHeatStep s;
  s.value = 800;
  s.units = Unit::Temp;
  s.duration = Duration(1);
  s.cleanup = Duration(2);
  s.pre_cleanup = Duration(3);
  s.post_cleanup = Duration(4);
  s.beam_diameter = 5;
  s.ramp_rate = 6;
  s.ramp = Duration(7);
  s.cryo_temp = 8;
  return s;
}

}  // namespace

TEST(StepHeatWrite, RoundTripsTheSpecExample) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  auto back = parse_step_heat(to_toml(*t), "laser-12");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, *t);
}

TEST(StepHeatWrite, RoundTripsEveryStepKey) {
  StepHeatTemplate t;
  t.name = "furnace";
  t.description = "quote \" and\nnewline";
  t.steps = {every_key_step()};
  auto back = parse_step_heat(to_toml(t), "furnace");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, t);
}

TEST(StepHeatWrite, WritingTwiceIsByteIdentical) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  const std::string once = to_toml(*t);
  auto back = parse_step_heat(once, "laser-12");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(to_toml(*back), once);
}

TEST(StepHeatWrite, AStepEqualToTheDefaultIsLeftOut) {
  StepHeatTemplate t;
  t.name = "laser-12";
  t.units = Unit::Watts;
  t.duration = Duration(30);
  t.cleanup = Duration(15);
  StepHeatStep a;
  a.value = 0.5;
  a.duration = Duration(30);
  a.units = Unit::Watts;
  a.cleanup = Duration(15);
  StepHeatStep b;
  b.value = 1.0;
  b.duration = Duration(45);
  b.units = Unit::Percent;
  b.cleanup = Duration(10);
  t.steps = {a, b};

  const std::string text = to_toml(t);
  const auto step_a = at(text, "[[steps]]");
  const auto step_b = at(text, "[[steps]]", step_a + 1);
  const std::string a_text = text.substr(step_a, step_b - step_a);
  const std::string b_text = text.substr(step_b);
  EXPECT_EQ(a_text, "[[steps]]\nvalue = 0.5\n\n");
  EXPECT_NE(b_text.find("duration = 45.0"), std::string::npos) << text;
  EXPECT_NE(b_text.find("units = \"percent\""), std::string::npos) << text;
  EXPECT_NE(b_text.find("cleanup = 10.0"), std::string::npos) << text;

  const auto n = normalized(t);
  EXPECT_EQ(n.steps[0].duration, std::nullopt);
  EXPECT_EQ(n.steps[0].units, std::nullopt);
  EXPECT_EQ(n.steps[0].cleanup, std::nullopt);
  EXPECT_EQ(n.steps[1], b);
  auto back = parse_step_heat(text, "laser-12");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, n);
}

TEST(StepHeatWrite, NothingIsClearedWithoutATemplateDefault) {
  StepHeatTemplate t;
  t.steps = {every_key_step()};
  EXPECT_EQ(normalized(t), t);
}

TEST(StepHeatWrite, KeysInSpecOrder) {
  StepHeatTemplate t;
  t.name = "furnace";
  t.description = "d";
  t.units = Unit::Watts;
  t.duration = Duration(30);
  t.cleanup = Duration(15);
  t.steps = {every_key_step()};
  const std::string text = to_toml(t);

  std::size_t pos = at(text, "[template]");
  for (const char* k : {"name = ", "description = ", "units = ", "duration = ", "cleanup = "}) {
    const auto next = at(text, std::string("\n") + k, pos);
    EXPECT_GT(next, pos) << k;
    pos = next;
  }
  pos = at(text, "[[steps]]");
  EXPECT_GT(pos, at(text, "\ncleanup = "));
  for (const char* k : {"value = ", "units = ", "duration = ", "cleanup = ", "pre_cleanup = ", "post_cleanup = ",
                        "beam_diameter = ", "ramp_rate = ", "ramp = ", "cryo_temp = "}) {
    const auto next = at(text, std::string("\n") + k, pos);
    EXPECT_GT(next, pos) << k;
    pos = next;
  }
}

TEST(StepHeatWrite, TemplateTableLayout) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  EXPECT_EQ(to_toml(*t),
            "[template]\n"
            "name = \"laser-12\"\n"
            "description = \"CO2, 12 steps, sanidine\"\n"
            "units = \"watts\"\n"
            "duration = 30.0\n"
            "cleanup = 15.0\n"
            "\n"
            "[[steps]]\n"
            "value = 0.5\n"
            "\n"
            "[[steps]]\n"
            "value = 1.0\n"
            "duration = 45.0\n"
            "\n"
            "[[steps]]\n"
            "value = 1.5\n"
            "beam_diameter = 2.0\n"
            "ramp_rate = 0.1\n");
}

TEST(StepHeatWrite, EmptyNameAndDescriptionAreLeftOut) {
  StepHeatTemplate t;
  StepHeatStep s;
  s.value = 2;
  t.steps = {s};
  EXPECT_EQ(to_toml(t), "[[steps]]\nvalue = 2.0\n");
}

TEST(StepHeatWrite, FloatsAreWrittenAsTheQueueWriterWritesThem) {
  StepHeatTemplate t;
  t.name = "x";
  t.duration = Duration(30);
  StepHeatStep s;
  s.value = 0.1;
  t.steps = {s};
  const std::string text = to_toml(t);
  EXPECT_NE(text.find("\nvalue = 0.1\n"), std::string::npos) << text;
  EXPECT_NE(text.find("\nduration = 30.0\n"), std::string::npos) << text;
}

// Spec 8: the precedence step > template > form, tested once with a table.
TEST(StepHeatPrecedence, StepThenTemplateThenForm) {
  ExtractionSpec form;
  form.units = Unit::Watts;
  form.duration = Duration(10);
  form.cleanup = Duration(5);
  form.pre_cleanup = Duration(1);
  form.beam_diameter = 1.0;
  form.device = "co2";
  form.script = "x";
  StepHeatTemplate t;
  t.units = Unit::Temp;
  t.duration = Duration(30);

  StepHeatStep a;
  a.value = 2;
  StepHeatStep b;
  b.value = 3;
  b.duration = Duration(45);
  b.cleanup = Duration(7);
  b.units = Unit::Percent;
  b.beam_diameter = 2.0;
  b.ramp = Duration(4);
  b.ramp_rate = 0.1;
  b.cryo_temp = 20;
  b.pre_cleanup = Duration(2);
  b.post_cleanup = Duration(3);

  struct Row {
    const char* name;
    const StepHeatStep* step;
    double value;
    Unit units;
    double duration, cleanup, pre_cleanup, post_cleanup, ramp;
    std::optional<double> beam_diameter, ramp_rate, cryo_temp;
  };
  const std::vector<Row> rows = {
      {"A", &a, 2, Unit::Temp, 30, 5, 1, 0, 0, 1.0, std::nullopt, std::nullopt},
      {"B", &b, 3, Unit::Percent, 45, 7, 2, 3, 4, 2.0, 0.1, 20.0},
  };
  for (const auto& row : rows) {
    SCOPED_TRACE(row.name);
    const ExtractionSpec e = resolve_step(form, t, *row.step);
    EXPECT_EQ(e.value, row.value);
    EXPECT_EQ(e.units, row.units);
    EXPECT_EQ(e.duration, Duration(row.duration));
    EXPECT_EQ(e.cleanup, Duration(row.cleanup));
    EXPECT_EQ(e.pre_cleanup, Duration(row.pre_cleanup));
    EXPECT_EQ(e.post_cleanup, Duration(row.post_cleanup));
    EXPECT_EQ(e.ramp, Duration(row.ramp));
    EXPECT_EQ(e.beam_diameter, row.beam_diameter);
    EXPECT_EQ(e.ramp_rate, row.ramp_rate);
    EXPECT_EQ(e.cryo_temp, row.cryo_temp);
    EXPECT_EQ(e.device, "co2");
    EXPECT_EQ(e.script, "x");
  }
}

TEST(StepHeatMake, GivesLettersValuesAndPerStepDurations) {
  auto t = parse_step_heat(kSpecExample, "laser-12");
  ASSERT_TRUE(t) << t.error().what;
  RunSpec tmpl;
  tmpl.id.identifier = "20001";
  const auto runs = make_step_heat(tmpl, *t);
  ASSERT_EQ(runs.size(), 3U);
  const std::vector<std::string> steps = {"A", "B", "C"};
  const std::vector<double> values = {0.5, 1.0, 1.5}, durations = {30, 45, 30};
  for (std::size_t i = 0; i < runs.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(runs[i].id.identifier, "20001");
    EXPECT_EQ(runs[i].id.step, steps[i]);
    EXPECT_EQ(runs[i].extraction.value, values[i]);
    EXPECT_EQ(runs[i].extraction.duration, Duration(durations[i]));
  }
  EXPECT_EQ(runs[2].extraction.beam_diameter, 2.0);
  EXPECT_EQ(runs[0].extraction.beam_diameter, std::nullopt);
}

TEST(StepHeatMake, TwentySevenStepsGoPastZ) {
  StepHeatTemplate t;
  for (int i = 0; i < 27; ++i) {
    StepHeatStep s;
    s.value = i + 1;
    t.steps.push_back(s);
  }
  const auto runs = make_step_heat(RunSpec{}, t);
  ASSERT_EQ(runs.size(), 27U);
  EXPECT_EQ(runs[25].id.step, "Z");
  EXPECT_EQ(runs[26].id.step, "AA");
}

TEST(StepHeatMake, TextPathGivesTodaysRuns) {
  FactoryForm form;
  form.identifier = "20001";
  form.units = Unit::Temp;
  form.duration_s = 20;
  form.cleanup_s = 8;
  RunSpec tmpl;
  tmpl.id.identifier = "20001";
  tmpl.extraction.units = Unit::Temp;
  tmpl.extraction.duration = Duration(20);
  tmpl.extraction.cleanup = Duration(8);
  tmpl.extraction.pre_cleanup = Duration(3);
  const std::vector<double> values = {5, 10, 15};

  const StepHeatTemplate t = template_from_values(values, form);
  EXPECT_TRUE(t.name.empty());
  EXPECT_EQ(t.units, Unit::Temp);
  EXPECT_EQ(t.duration, Duration(20));
  EXPECT_EQ(t.cleanup, Duration(8));
  ASSERT_EQ(t.steps.size(), 3U);
  StepHeatStep only_value;
  only_value.value = 10;
  EXPECT_EQ(t.steps[1], only_value);
  EXPECT_EQ(make_step_heat(tmpl, values), make_step_heat(tmpl, t));
}

TEST(StepHeatMake, ExtractionOfTakesTheFormsHeatingFields) {
  FactoryForm form;
  form.value = 4;
  form.units = Unit::Percent;
  form.duration_s = 12;
  form.cleanup_s = 6;
  form.extract_device = "co2";
  form.script = "x";
  ExtractionSpec want;
  want.value = 4;
  want.units = Unit::Percent;
  want.duration = Duration(12);
  want.cleanup = Duration(6);
  EXPECT_EQ(extraction_of(form), want);
}

TEST(StepHeatMake, ExtractionTimeSumsTheSteps) {
  ExtractionSpec form;
  form.pre_cleanup = Duration(0);
  StepHeatTemplate t;
  t.duration = Duration(30);
  t.cleanup = Duration(15);
  StepHeatStep s1;
  s1.value = 1;
  StepHeatStep s2;
  s2.value = 2;
  s2.duration = Duration(60);
  t.steps = {s1, s2};
  EXPECT_EQ(extraction_time(form, t), Duration(120));
}

namespace {

StepHeatTemplate steps_of(const std::vector<double>& values) {
  StepHeatTemplate t;
  t.name = "edit";
  for (const double v : values) {
    StepHeatStep s;
    s.value = v;
    t.steps.push_back(s);
  }
  return t;
}

// `n` runs A, B, ... with values spread evenly from `lo` to `hi`, each `seconds` of extraction.
std::vector<RunSpec> summary_runs(std::size_t n, double lo, double hi, double seconds) {
  std::vector<RunSpec> runs(n);
  for (std::size_t i = 0; i < n; ++i) {
    runs[i].id.step = step_name(static_cast<int>(i));
    runs[i].extraction.value = n == 1 ? lo : lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n - 1);
    runs[i].extraction.units = Unit::Watts;
    runs[i].extraction.duration = Duration(seconds);
  }
  return runs;
}

}  // namespace

TEST(StepHeatEdit, StepAfterAddsTheLastIncrement) {
  StepHeatTemplate t = steps_of({1.0, 2.5});
  t.steps[1].duration = Duration(45);
  t.steps[1].beam_diameter = 2.0;
  const StepHeatStep s = step_after(t, 1);
  EXPECT_DOUBLE_EQ(s.value, 4.0);
  EXPECT_EQ(s.duration, Duration(45));
  EXPECT_EQ(s.beam_diameter, 2.0);
  StepHeatStep expected = t.steps[1];
  expected.value = s.value;
  EXPECT_EQ(s, expected);
}

TEST(StepHeatEdit, StepAfterOneStepCopiesIt) {
  StepHeatTemplate t = steps_of({3.0});
  t.steps[0].duration = Duration(45);
  const StepHeatStep s = step_after(t, 0);
  EXPECT_DOUBLE_EQ(s.value, 3.0);
  EXPECT_EQ(s.duration, Duration(45));
}

TEST(StepHeatEdit, StepAfterNothingIsOneWatt) {
  const StepHeatStep s = step_after(StepHeatTemplate{}, 7);
  StepHeatStep one_watt;
  one_watt.value = 1;
  EXPECT_EQ(s, one_watt);
}

TEST(StepHeatEdit, FillValuesSetsSelectedRows) {
  StepHeatTemplate t = steps_of({1, 2, 3});
  ASSERT_TRUE(fill_values(t, {0, 2}, {7, 9}));
  ASSERT_EQ(t.steps.size(), 3u);
  EXPECT_DOUBLE_EQ(t.steps[0].value, 7);
  EXPECT_DOUBLE_EQ(t.steps[1].value, 2);
  EXPECT_DOUBLE_EQ(t.steps[2].value, 9);
}

TEST(StepHeatEdit, FillValuesAppendsWithoutSelection) {
  StepHeatTemplate t = steps_of({1, 2});
  t.steps[1].cleanup = Duration(20);
  ASSERT_TRUE(fill_values(t, {}, {5, 6}));
  ASSERT_EQ(t.steps.size(), 4u);
  EXPECT_DOUBLE_EQ(t.steps[2].value, 5);
  EXPECT_DOUBLE_EQ(t.steps[3].value, 6);
  EXPECT_EQ(t.steps[3].cleanup, Duration(20));  // copied from the step before, as Add does
}

TEST(StepHeatEdit, FillValuesRefusesACountMismatch) {
  StepHeatTemplate t = steps_of({1, 2, 3});
  const StepHeatTemplate before = t;
  const auto r = fill_values(t, {0, 1}, {4, 5, 6});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().what, "3 values for 2 selected steps");
  EXPECT_EQ(t, before);
}

TEST(StepHeatEdit, FillValuesRefusesMoreThan1000Steps) {
  StepHeatTemplate t = steps_of(std::vector<double>(999, 1.0));
  const StepHeatTemplate before = t;
  const auto r = fill_values(t, {}, {2, 3});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(t, before);
  ASSERT_TRUE(fill_values(t, {}, {2}));
  EXPECT_EQ(t.steps.size(), kMaxStepHeatSteps);
}

TEST(StepHeatSummary, SaysCountStepsRangeAndTime) {
  EXPECT_EQ(step_heat_summary(summary_runs(12, 0.5, 6, 105)),
            "12 runs, steps A to L, 0.5 to 6 W, 21 min extraction");
}

TEST(StepHeatSummary, OneRunAndLongSchedules) {
  EXPECT_EQ(step_heat_summary(summary_runs(1, 5, 5, 45)), "1 run, step A, 5 W, 45 s extraction");
  EXPECT_EQ(step_heat_summary(summary_runs(1, 5, 5, 3900)), "1 run, step A, 5 W, 1 h 5 min extraction");
}

TEST(StepHeatSummary, UnitsOfTheFirstRunAndNothingForNoRuns) {
  auto runs = summary_runs(2, 600, 900, 30);
  for (auto& r : runs) r.extraction.units = Unit::Temp;
  EXPECT_EQ(step_heat_summary(runs), "2 runs, steps A to B, 600 to 900 °C, 1 min extraction");
  EXPECT_EQ(step_heat_summary({}), "");
}

namespace {

namespace fs = std::filesystem;

class ScratchDir {
 public:
  ScratchDir() : dir_(nobion::testing::unique_temp_path("pychron-step-heats-")) { fs::create_directories(dir_); }
  ~ScratchDir() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;
  ScratchDir(ScratchDir&&) = delete;
  ScratchDir& operator=(ScratchDir&&) = delete;
  [[nodiscard]] const fs::path& path() const { return dir_; }

 private:
  fs::path dir_;
};

std::vector<std::string> entries(const fs::path& dir) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
  std::ranges::sort(out);
  return out;
}

StepHeatTemplate named(std::string name, const std::vector<double>& values) {
  StepHeatTemplate t;
  t.name = std::move(name);
  t.units = Unit::Watts;
  t.duration = Duration(30);
  for (const double v : values) {
    StepHeatStep s;
    s.value = v;
    s.duration = Duration(30);  // equal to the default: normalized clears it
    t.steps.push_back(s);
  }
  return t;
}

}  // namespace

TEST(StepHeatFiles, WriteReadListRemove) {
  const ScratchDir scratch;
  const StepHeatFiles files(scratch.path());
  const auto a = named("a", {0.5, 1});
  ASSERT_TRUE(files.write(named("b", {2})));
  ASSERT_TRUE(files.write(a));
  auto names = files.list();
  ASSERT_TRUE(names) << names.error().what;
  EXPECT_EQ(*names, (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(files.exists("a"));
  EXPECT_EQ(files.path("a"), scratch.path() / "a.toml");
  auto back = files.read("a");
  ASSERT_TRUE(back) << back.error().what;
  EXPECT_EQ(*back, normalized(a));
  ASSERT_TRUE(files.remove("a"));
  EXPECT_FALSE(files.exists("a"));
  EXPECT_EQ(*files.list(), (std::vector<std::string>{"b"}));
  EXPECT_EQ(entries(scratch.path()), (std::vector<std::string>{"b.toml"}));  // no temporary file left
}

TEST(StepHeatFiles, WriteCreatesTheDirectory) {
  const ScratchDir scratch;
  const StepHeatFiles files(scratch.path() / "queues" / "step_heats");
  EXPECT_TRUE(files.list()->empty());  // missing directory: no names, no error
  ASSERT_TRUE(files.write(named("x", {1})));
  EXPECT_TRUE(fs::is_regular_file(scratch.path() / "queues" / "step_heats" / "x.toml"));
}

TEST(StepHeatFiles, WriteRefusesABadTemplate) {
  const ScratchDir scratch;
  const StepHeatFiles files(scratch.path());
  const auto r = files.write(named("x", {0}));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_TRUE(entries(scratch.path()).empty());
}

TEST(StepHeatFiles, RefusesNamesOutsideTheRule) {
  const ScratchDir scratch;
  const auto dir = scratch.path() / "step_heats";
  const StepHeatFiles files(dir);
  const auto before = entries(scratch.path());
  for (const std::string name : {"../x", "a/b", "Laser", ""}) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(files.exists(name));
    const auto w = files.write(named(name, {1}));
    ASSERT_FALSE(w);
    EXPECT_EQ(w.error().kind, ErrorKind::Config);
    const auto r = files.read(name);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
    const auto d = files.remove(name);
    ASSERT_FALSE(d);
    EXPECT_EQ(d.error().kind, ErrorKind::Config);
  }
  EXPECT_EQ(entries(scratch.path()), before);
  EXPECT_FALSE(fs::exists(dir));
}

TEST(StepHeatLoadDir, MissingDirectoryIsEmpty) {
  const ScratchDir scratch;
  const auto load = load_step_heats(scratch.path() / "absent");
  EXPECT_TRUE(load.templates.empty());
  EXPECT_TRUE(load.problems.empty());
}

TEST(StepHeatLoadDir, BadFilesAreProblemsGoodOnesLoad) {
  const ScratchDir scratch;
  const auto& dir = scratch.path();
  std::ofstream(dir / "good.toml") << "[[steps]]\nvalue = 1\n";
  std::ofstream(dir / "bad.toml") << "[[steps]]\nvalue = 0\n";
  std::ofstream(dir / "Bad Name.toml") << "[[steps]]\nvalue = 1\n";
  std::ofstream(dir / "notes.txt") << "not a template\n";
  const auto load = load_step_heats(dir);
  ASSERT_EQ(load.templates.size(), 1u);
  EXPECT_TRUE(load.templates.contains("good"));
  ASSERT_EQ(load.problems.size(), 2u);
  // Sorted by path: "Bad Name.toml" before "bad.toml".
  EXPECT_EQ(load.problems[0],
            (dir / "Bad Name.toml").string() + ": name 'Bad Name' must be lower-case letters, digits, '_' or '-'");
  EXPECT_EQ(load.problems[1].rfind((dir / "bad.toml").string() + ": ", 0), 0u) << load.problems[1];
}
