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

  // Waits, at most kBound, until `n` hang alarms have come.
  bool await_alarms(std::size_t n) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kBound, [&] { return alarms_.size() >= n; });
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

namespace {

// A gate a test thread opens: what waits at it waits at most kBound.
class Gate {
 public:
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, kBound, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

// Opens the gate on every way out, so nothing stays blocked at it.
class OpenOnExit {
 public:
  explicit OpenOnExit(Gate& gate) : gate_(gate) {}
  ~OpenOnExit() { gate_.open(); }
  OpenOnExit(const OpenOnExit&) = delete;
  OpenOnExit& operator=(const OpenOnExit&) = delete;
  OpenOnExit(OpenOnExit&&) = delete;
  OpenOnExit& operator=(OpenOnExit&&) = delete;

 private:
  Gate& gate_;
};

// A recorder whose request() says what the ring and the log path had seen
// when it came, and may block until a gate opens.
class Witness final : public IDumpRequester {
 public:
  Witness(LogHub& hub, Gate* hold) : hub_(hub), hold_(hold) {}

  void request(std::string /*reason*/) override {
    {
      const std::lock_guard lock(mutex_);
      ring_had_it_ = count_of(hub_.recorder_snapshot(), "hang: b silent") == 1;
      log_had_it_ = logged_;
      in_request_ = true;
    }
    cv_.notify_all();
    if (hold_ != nullptr) hold_->wait();
  }

  void logged() {
    const std::lock_guard lock(mutex_);
    logged_ = true;
  }

  bool await_request() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kBound, [this] { return in_request_; });
  }
  bool ring_had_it() {
    const std::lock_guard lock(mutex_);
    return ring_had_it_;
  }
  bool log_had_it() {
    const std::lock_guard lock(mutex_);
    return log_had_it_;
  }

 private:
  LogHub& hub_;
  Gate* hold_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool logged_ = false;
  bool in_request_ = false;
  bool ring_had_it_ = false;
  bool log_had_it_ = false;
};

std::shared_ptr<LogHub> make_hub_on(const Clock& clock, SignalBus& bus) {
  config::LoggingConfig cfg;
  cfg.dir = std::filesystem::path{};
  cfg.dock_level = LogLevel::Info;
  auto hub = LogHub::create(cfg, clock, &bus);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

}  // namespace

// A log path that does not return (a hung disk under an `error` record; here
// a Log handler that blocks) does not keep the dump from being asked for: the
// record is in the ring first, then the dump, then the write that hangs.
TEST(HangMonitor, TheDumpIsAskedForBeforeTheLogWriteThatMayHang) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub_on(clock, bus);
  ASSERT_TRUE(hub);
  Gate log_path;
  Witness witness(*hub, nullptr);
  const auto stuck = bus.subscribe<Log>([&](const Log& e) {
    if (e.logger != "hang") return;
    witness.logged();
    log_path.wait();
  });
  Reports reports(bus);
  HangMonitor monitor(*hub, witness, bus, HangMonitor::Options{50ms});
  const HangMonitor::DetachOnExit detach(monitor);
  const OpenOnExit release(log_path);  // after the detach: opened before it waits
  const auto b = monitor.beat("b", 50ms);

  ASSERT_TRUE(witness.await_request()) << "no dump asked for while the log write hangs";
  EXPECT_TRUE(witness.ring_had_it()) << "the ring did not have the record when the dump was asked for";
  EXPECT_FALSE(witness.log_had_it()) << "the log write came before the dump";
  log_path.open();
  ASSERT_TRUE(reports.await_alarms(1));
  EXPECT_EQ(count_of(hub->recorder_snapshot(), "hang: b silent"), 1U);  // once, not again from the log write
}

// detach() waits for a report in progress at most detach_wait, says it gave
// up, and the report it left makes no call to the bus once its stuck call
// returns.
TEST(HangMonitor, DetachGivesUpOnAStuckReportWithinItsBound) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(clock);
  ASSERT_TRUE(hub);
  Gate recorder_call;
  Witness witness(*hub, &recorder_call);
  Reports reports(bus);
  HangMonitor monitor(*hub, witness, bus, HangMonitor::Options{50ms, 200ms});
  const OpenOnExit release(recorder_call);  // after the monitor: opened before it joins
  const auto b = monitor.beat("b", 50ms);
  ASSERT_TRUE(witness.await_request());

  const auto started = std::chrono::steady_clock::now();
  monitor.detach();
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_GE(took, 200ms);
  EXPECT_LT(took, kBound);
  EXPECT_EQ(count_of(hub->recorder_snapshot(), "detach gave up waiting for a report in progress"), 1U);

  recorder_call.open();
  std::this_thread::sleep_for(300ms);  // six polls: room for a publish that must not come
  EXPECT_TRUE(reports.alarms().empty());
}
