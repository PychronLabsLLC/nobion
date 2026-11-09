#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "metrics_text.hpp"
#include "nobion/metrics/registry.hpp"

using namespace nobion::metrics;
using metrics_text::has;
using metrics_text::value;

namespace {
const char* const kDropped = "nobion_metrics_dropped_series_total";
}

TEST(Registry, SameNameAndLabelsIsOneSeries) {
  Registry r;
  r.counter("nobion_x_total", "h", {{"a", "1"}}).inc();
  r.counter("nobion_x_total", "h", {{"a", "1"}}).inc(2);
  EXPECT_DOUBLE_EQ(r.counter("nobion_x_total", "h", {{"a", "1"}}).value(), 3.0);
  EXPECT_DOUBLE_EQ(r.counter("nobion_x_total", "h", {{"a", "2"}}).value(), 0.0);
}

TEST(Registry, LabelOrderDoesNotMakeANewSeries) {
  Registry r;
  r.gauge("nobion_g", "h", {{"a", "1"}, {"b", "2"}}).set(4);
  EXPECT_DOUBLE_EQ(r.gauge("nobion_g", "h", {{"b", "2"}, {"a", "1"}}).value(), 4.0);
}

TEST(Registry, CounterIgnoresANegativeIncrement) {
  Registry r;
  Counter& c = r.counter("nobion_x_total", "h");
  c.inc(2);
  c.inc(-1);
  c.inc(std::nan(""));
  EXPECT_DOUBLE_EQ(c.value(), 2.0);
}

TEST(Registry, GaugeMoves) {
  Registry r;
  Gauge& g = r.gauge("nobion_g", "h");
  g.set(5);
  g.inc();
  g.dec(2.5);
  EXPECT_DOUBLE_EQ(g.value(), 3.5);
}

TEST(Registry, SetTotalAddsTheIncrease) {
  Registry r;
  Counter& c = r.counter("nobion_x_total", "h");
  c.set_total(5);
  c.set_total(8);
  EXPECT_DOUBLE_EQ(c.value(), 8.0);
}

TEST(Registry, SetTotalTreatsADropAsARestart) {
  Registry r;
  Counter& c = r.counter("nobion_x_total", "h");
  c.set_total(5);
  c.set_total(2);
  EXPECT_DOUBLE_EQ(c.value(), 7.0);
  c.set_total(3);
  EXPECT_DOUBLE_EQ(c.value(), 8.0);
}

TEST(Registry, ANameReusedAsAnotherTypeIsNotRendered) {
  Registry r;
  r.counter("nobion_x_total", "h").inc();
  r.gauge("nobion_x_total", "h").set(9);
  const std::string text = r.render();
  EXPECT_DOUBLE_EQ(value(text, "nobion_x_total"), 1.0);
  EXPECT_DOUBLE_EQ(value(text, kDropped), 1.0);
}

TEST(Registry, AnInvalidNameIsNotRendered) {
  Registry r;
  r.gauge("9bad", "h").set(1);
  r.gauge("has space", "h").set(1);
  r.gauge("", "h").set(1);
  r.gauge("nobion_g", "h", {{"bad-label", "v"}}).set(1);
  r.histogram("nobion_h", "h", {1.0}, {{"le", "v"}}).observe(1);
  const std::string text = metrics_text::without_own(r.render());
  EXPECT_EQ(text, "");
  EXPECT_DOUBLE_EQ(value(r.render(), kDropped), 5.0);
}

TEST(Registry, HistogramBucketsAreCumulative) {
  Registry r;
  Histogram& h = r.histogram("nobion_h", "h", {1.0, 5.0});
  h.observe(0.5);
  h.observe(3);
  h.observe(100);
  EXPECT_EQ(h.count(), 3u);
  EXPECT_DOUBLE_EQ(h.sum(), 103.5);
  const std::string text = r.render();
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_bucket{le=\"1\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_bucket{le=\"5\"}"), 2.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_bucket{le=\"+Inf\"}"), 3.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_count"), 3.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_sum"), 103.5);
}

TEST(Registry, AValueOnABucketBoundaryIsInThatBucket) {
  Registry r;
  r.histogram("nobion_h", "h", {1.0, 5.0}).observe(1.0);
  EXPECT_DOUBLE_EQ(value(r.render(), "nobion_h_bucket{le=\"1\"}"), 1.0);
}

TEST(Registry, HistogramBucketsAreSortedAndLabelled) {
  Registry r;
  r.histogram("nobion_h", "h", {5.0, 1.0}, {{"state", "saving"}}).observe(2);
  const std::string text = r.render();
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_bucket{state=\"saving\",le=\"1\"}"), 0.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_bucket{state=\"saving\",le=\"5\"}"), 1.0);
  EXPECT_DOUBLE_EQ(value(text, "nobion_h_sum{state=\"saving\"}"), 2.0);
}

TEST(Registry, AFamilyStopsAtItsCap) {
  Registry r;
  for (std::size_t i = 0; i < Registry::kMaxSeriesPerFamily + 5; ++i) {
    r.gauge("nobion_g", "h", {{"n", std::to_string(i)}}).set(1);
  }
  const std::string text = r.render();
  std::size_t series = 0;
  for (const std::string& line : metrics_text::lines(text)) {
    if (line.starts_with("nobion_g{")) ++series;
  }
  EXPECT_EQ(series, Registry::kMaxSeriesPerFamily);
  EXPECT_DOUBLE_EQ(value(text, kDropped), 5.0);
  // A series that already exists is still found.
  EXPECT_DOUBLE_EQ(r.gauge("nobion_g", "h", {{"n", "0"}}).value(), 1.0);
}

TEST(Registry, ConcurrentIncrementsSumExactly) {
  Registry r;
  Counter& shared = r.counter("nobion_x_total", "h");
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&r, &shared, t] {
      for (int i = 0; i < 10000; ++i) {
        shared.inc();
        r.counter("nobion_y_total", "h", {{"t", std::to_string(t)}}).inc();
        r.histogram("nobion_h", "h", {1.0}).observe(0.5);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_DOUBLE_EQ(shared.value(), 80000.0);
  for (int t = 0; t < 8; ++t) {
    EXPECT_DOUBLE_EQ(r.counter("nobion_y_total", "h", {{"t", std::to_string(t)}}).value(), 10000.0);
  }
  EXPECT_EQ(r.histogram("nobion_h", "h", {1.0}).count(), 80000u);
  EXPECT_DOUBLE_EQ(r.histogram("nobion_h", "h", {1.0}).sum(), 40000.0);
}

TEST(Registry, RenderingWhileUpdatingIsSafe) {
  Registry r;
  std::thread writer([&r] {
    for (int i = 0; i < 2000; ++i) r.gauge("nobion_g", "h", {{"n", std::to_string(i % 50)}}).set(i);
  });
  for (int i = 0; i < 200; ++i) (void)r.render();
  writer.join();
  EXPECT_TRUE(has(r.render(), "nobion_g{n=\"0\"}"));
}

TEST(Registry, ACollectorRunsAtEachRenderUntilItsHandleIsReset) {
  Registry r;
  int runs = 0;
  CollectorHandle h = r.add_collector([&runs](Registry& reg) {
    ++runs;
    reg.gauge("nobion_g", "h").set(runs);
  });
  EXPECT_DOUBLE_EQ(value(r.render(), "nobion_g"), 1.0);
  EXPECT_DOUBLE_EQ(value(r.render(), "nobion_g"), 2.0);
  h.reset();
  (void)r.render();
  EXPECT_EQ(runs, 2);
}

TEST(Registry, ACollectorThatThrowsDoesNotStopTheRender) {
  Registry r;
  r.gauge("nobion_g", "h").set(1);
  CollectorHandle h = r.add_collector([](Registry&) { throw std::runtime_error("no"); });
  EXPECT_DOUBLE_EQ(value(r.render(), "nobion_g"), 1.0);
}

TEST(Registry, ACollectorHandleMayOutliveTheRegistry) {
  CollectorHandle h;
  {
    Registry r;
    h = r.add_collector([](Registry&) {});
  }
  h.reset();  // must not touch the dead registry
  SUCCEED();
}

TEST(Registry, RemoveHidesASeriesAndSetBringsItBack) {
  Registry r;
  r.gauge("nobion_g", "h", {{"h", "a"}}).set(1);
  r.remove("nobion_g", {{"h", "a"}});
  EXPECT_FALSE(has(r.render(), "nobion_g{h=\"a\"}"));
  r.gauge("nobion_g", "h", {{"h", "a"}}).set(2);
  EXPECT_DOUBLE_EQ(value(r.render(), "nobion_g{h=\"a\"}"), 2.0);
}

TEST(Registry, RemovingWhatIsNotThereIsHarmless) {
  Registry r;
  r.remove("nobion_nothing", {});
  r.gauge("nobion_g", "h").set(1);
  r.remove("nobion_g", {{"h", "a"}});
  EXPECT_TRUE(has(r.render(), "nobion_g"));
}

TEST(Registry, NamesListsFamilies) {
  Registry r;
  r.gauge("nobion_b", "h").set(1);
  r.counter("nobion_a_total", "h", {{"x", "1"}});
  const std::vector<std::string> names = r.names();
  EXPECT_EQ(names, (std::vector<std::string>{"nobion_a_total", "nobion_b", kDropped}));
}

TEST(Registry, TheDefaultClockMovesForwardInRealTime) {
  const RealClock clock = steady_real_clock();
  const double a = clock();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const double b = clock();
  EXPECT_GE(b - a, 0.015);
  EXPECT_LT(b - a, 5.0);
}
