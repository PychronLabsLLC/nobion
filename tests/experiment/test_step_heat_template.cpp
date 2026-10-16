#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "pychron/experiment/factory/step_heat_template.hpp"

using namespace pychron;
using namespace pychron::experiment;

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
