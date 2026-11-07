#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/hang_monitor.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/signal_bus.hpp"

// Spec 2026-11-06-field-diagnostics, 6. Real time, on purpose: the monitor is
// on real time (virtual clock spec 4.4). Limits are short; every wait for a
// report has an upper bound of 5 s, there only to tell a report from none.

using namespace nobion;
using namespace std::chrono_literals;

namespace {

constexpr auto kBound = 5s;

// Hub with no log file: records reach the recorder's ring, read back whole.
std::shared_ptr<LogHub> make_hub(const Clock& clock) {
  config::LoggingConfig cfg;
  cfg.dir = std::filesystem::path{};
  auto hub = LogHub::create(cfg, clock);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

std::size_t count_of(const std::string& text, const std::string& what) {
  std::size_t n = 0;
  for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

// What the monitor reported: dump requests and hang alarms, as they come.
class Reports final : public IDumpRequester {
 public:
  explicit Reports(SignalBus& bus)
      : alarm_(bus.subscribe<Alarm>([this](const Alarm& a) {
          if (a.source != "hang") return;
          {
            const std::lock_guard lock(mutex_);
            alarms_.push_back(a);
          }
          cv_.notify_all();
        })) {}

  void request(std::string reason) override {
    {
      const std::lock_guard lock(mutex_);
      requests_.push_back(std::move(reason));
    }
    cv_.notify_all();
  }

  // Waits, at most kBound, until `reason` has been asked for `n` times and as
  // many hang alarms have come.
  bool await(const std::string& reason, std::size_t n) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kBound, [&] { return count_locked(reason) >= n && alarms_.size() >= n; });
  }

  std::size_t requests(const std::string& reason) {
    const std::lock_guard lock(mutex_);
    return count_locked(reason);
  }
  std::vector<Alarm> alarms() {
    const std::lock_guard lock(mutex_);
    return alarms_;
  }

 private:
  std::size_t count_locked(const std::string& reason) const {
    std::size_t n = 0;
    for (const auto& r : requests_) n += r == reason ? 1U : 0U;
    return n;
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> requests_;
  std::vector<Alarm> alarms_;
  SignalBus::Subscription alarm_;
};

}  // namespace

TEST(HangMonitor, ASilentBeatReportsOnce) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  const auto b = monitor.beat("b", 200ms);

  ASSERT_TRUE(reports.await("hang-b", 1));
  std::this_thread::sleep_for(1s);  // what a second report would need, many times over
  EXPECT_EQ(reports.requests("hang-b"), 1U);
  EXPECT_EQ(count_of(hub->recorder_snapshot(), "hang: b silent"), 1U);
  const auto alarms = reports.alarms();
  ASSERT_EQ(alarms.size(), 1U);
  EXPECT_EQ(alarms[0].severity, AlarmSeverity::Warning);
  EXPECT_NE(alarms[0].message.find("hang: b silent for "), std::string::npos) << alarms[0].message;
}

TEST(HangMonitor, APulseRearms) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  auto b = monitor.beat("b", 200ms);

  ASSERT_TRUE(reports.await("hang-b", 1));
  b.pulse();
  ASSERT_TRUE(reports.await("hang-b", 2));
  EXPECT_EQ(count_of(hub->recorder_snapshot(), "hang: b silent"), 2U);
}

TEST(HangMonitor, ADisarmedBeatNeverReports) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  auto quiet = monitor.beat("quiet", 50ms);
  quiet.disarm();
  const auto control = monitor.beat("control", 50ms);

  ASSERT_TRUE(reports.await("hang-control", 1));
  EXPECT_EQ(reports.requests("hang-quiet"), 0U);
  EXPECT_EQ(count_of(hub->recorder_snapshot(), "hang: quiet"), 0U);
}

// A beat let go is no longer watched, and one that outlives its monitor is
// harmless.
TEST(HangMonitor, ABeatLetGoIsForgottenAndMayOutliveTheMonitor) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor::Beat survivor;
  {
    HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
    { const auto gone = monitor.beat("gone", 50ms); }
    survivor = monitor.beat("survivor", 1h);
    const auto control = monitor.beat("control", 50ms);
    ASSERT_TRUE(reports.await("hang-control", 1));
    EXPECT_EQ(reports.requests("hang-gone"), 0U);
  }
  survivor.pulse();
  survivor.disarm();
}

// Late attach: a monitor made before what it reports to watches all along and
// reports a beat already silent once attached, stamped by the clock given.
TEST(HangMonitor, ABeatSilentBeforeAttachIsReportedOnceAttached) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(HangMonitor::Options{50ms});
  const auto b = monitor.beat("b", 50ms);
  std::this_thread::sleep_for(500ms);  // ten polls, all past the limit, with nothing attached
  EXPECT_EQ(reports.requests("hang-b"), 0U);

  const ManualClock stamps(TimePoint{} + 42s);
  monitor.attach(*hub, reports, bus, &stamps);
  ASSERT_TRUE(reports.await("hang-b", 1));
  const auto alarms = reports.alarms();
  ASSERT_EQ(alarms.size(), 1U);
  EXPECT_EQ(alarms[0].ts, TimePoint{} + 42s);
  monitor.detach();
}

TEST(HangMonitor, DetachStopsReports) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  monitor.detach();
  const auto b = monitor.beat("b", 50ms);
  std::this_thread::sleep_for(500ms);  // ten polls, all past the limit
  EXPECT_EQ(reports.requests("hang-b"), 0U);
  EXPECT_TRUE(reports.alarms().empty());
}

// A beat made disarmed is not watched until its first pulse.
TEST(HangMonitor, ABeatMadeDisarmedWaitsForItsFirstPulse) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  auto quiet = monitor.beat("quiet", 50ms, false);
  const auto control = monitor.beat("control", 50ms);
  ASSERT_TRUE(reports.await("hang-control", 1));
  EXPECT_EQ(reports.requests("hang-quiet"), 0U);
  quiet.pulse();
  ASSERT_TRUE(reports.await("hang-quiet", 1));
}

// A pulse with a limit of its own holds for that episode only.
TEST(HangMonitor, APulseLimitHoldsForItsEpisode) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Reports reports(bus);
  HangMonitor monitor(*hub, reports, bus, HangMonitor::Options{50ms});
  auto b = monitor.beat("b", 50ms, false);
  b.pulse(1h);
  const auto control = monitor.beat("control", 50ms);
  ASSERT_TRUE(reports.await("hang-control", 1));
  EXPECT_EQ(reports.requests("hang-b"), 0U);
  b.pulse();  // back to the beat's own 50 ms
  ASSERT_TRUE(reports.await("hang-b", 1));
}
