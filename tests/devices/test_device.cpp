#include "nobion/devices/device.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <thread>

#include "log_capture.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/signal_bus.hpp"

using namespace nobion;
using namespace std::chrono_literals;

namespace {

class Plain : public Device {
 public:
  using Device::Device;
  using Device::begin;
  using Device::call;
  using Device::observe;
  using Device::relay;

  // One operation, observed without a name of its own in the test.
  template <class T>
  Result<T> observe(Result<T> result) {
    return observe(begin("op"), std::move(result));
  }
};

bool has_line(const std::vector<std::string>& lines, const std::string& wanted) {
  return std::ranges::find(lines, wanted) != lines.end();
}

}  // namespace

TEST(Device, HasNameAndStartsUnknown) {
  Plain d("ig1");
  EXPECT_EQ(d.name(), "ig1");
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Unknown);
  EXPECT_EQ(h.consecutive_failures, 0u);
  EXPECT_FALSE(h.last_error.has_value());
  EXPECT_EQ(h.last_ok, TimePoint{});
}

TEST(Device, SuccessStampsLastOkFromClock) {
  ManualClock clock(TimePoint{} + 5s);
  Plain d("ig1", DeviceOptions{&clock, 3});
  Result<double> r = d.observe(Result<double>(1.5));
  ASSERT_TRUE(r);
  EXPECT_DOUBLE_EQ(*r, 1.5);
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Ok);
  EXPECT_EQ(h.last_ok, TimePoint{} + 5s);
}

TEST(Device, FailuresDegradeThenFault) {
  ManualClock clock;
  Plain d("ig1", DeviceOptions{&clock, 2});
  (void)d.observe(Result<void>{});
  (void)d.observe(Result<void>(fail(ErrorKind::Timeout, "no reply")));
  EXPECT_EQ(d.health().state, DeviceState::Degraded);
  EXPECT_EQ(d.health().consecutive_failures, 1u);
  EXPECT_EQ(d.health().last_error, ErrorKind::Timeout);

  (void)d.observe(Result<void>(fail(ErrorKind::Protocol, "garbled")));
  EXPECT_EQ(d.health().state, DeviceState::Faulted);
  EXPECT_EQ(d.health().consecutive_failures, 2u);
  EXPECT_EQ(d.health().last_error, ErrorKind::Protocol);
}

TEST(Device, SuccessResetsFailureCountButKeepsLastError) {
  Plain d("ig1", DeviceOptions{nullptr, 1});
  (void)d.observe(Result<int>(fail(ErrorKind::Io, "unplugged")));
  EXPECT_EQ(d.health().state, DeviceState::Faulted);
  (void)d.observe(Result<int>(3));
  DeviceHealth h = d.health();
  EXPECT_EQ(h.state, DeviceState::Ok);
  EXPECT_EQ(h.consecutive_failures, 0u);
  EXPECT_EQ(h.last_error, ErrorKind::Io);
  EXPECT_NE(h.last_ok, TimePoint{});  // SteadyClock used when none is injected
}

TEST(Device, ObserveAttributesUnnamedErrorsToDevice) {
  Plain d("ig1");
  Result<int> r = d.observe(Result<int>(fail(ErrorKind::NotConnected, "port closed")));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().device, "ig1");
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);

  Result<void> named = d.observe(Result<void>(fail(ErrorKind::Io, "x", "valve_bus")));
  EXPECT_EQ(named.error().device, "valve_bus");
}

TEST(Device, CancelledDoesNotCountAsDeviceFailure) {
  Plain d("ig1");
  (void)d.observe(Result<void>{});
  (void)d.observe(Result<void>(fail(ErrorKind::Cancelled, "shutting down")));
  EXPECT_EQ(d.health().state, DeviceState::Ok);
  EXPECT_EQ(d.health().consecutive_failures, 0u);
}

TEST(Device, StateNames) {
  EXPECT_EQ(to_string(DeviceState::Unknown), "unknown");
  EXPECT_EQ(to_string(DeviceState::Ok), "ok");
  EXPECT_EQ(to_string(DeviceState::Degraded), "degraded");
  EXPECT_EQ(to_string(DeviceState::Faulted), "faulted");
}

// ---- health on the bus (spec 2026-11-06-field-diagnostics, 7) -----------

namespace {

// Every DeviceHealthChanged the bus carries, in order.
struct HealthEvents {
  explicit HealthEvents(SignalBus& bus)
      : sub(bus.subscribe<DeviceHealthChanged>([this](const DeviceHealthChanged& e) { seen.push_back(e); })) {}
  std::vector<DeviceHealthChanged> seen;
  SignalBus::Subscription sub;
};

Result<void> failure(int i) { return fail(ErrorKind::Timeout, "no reply " + std::to_string(i)); }

}  // namespace

// fault_after failures: one event to Degraded (the first failure), one to
// Faulted (the fault_after-th), none for the failures in between or after.
TEST(Plain, FaultAfterPublishesOnceToFaulted) {
  SignalBus bus;
  HealthEvents events(bus);
  ManualClock clock(TimePoint{} + 7s);
  Plain d("ig1", DeviceOptions{&clock, 3});
  d.set_health_bus(&bus, "gauges");
  for (int i = 1; i <= 5; ++i) (void)d.observe(failure(i));

  ASSERT_EQ(events.seen.size(), 2u);
  const DeviceHealthChanged& degraded = events.seen[0];
  EXPECT_EQ(degraded.device, "ig1");
  EXPECT_EQ(degraded.transport, "gauges");
  EXPECT_EQ(degraded.from, DeviceState::Unknown);
  EXPECT_EQ(degraded.to, DeviceState::Degraded);
  EXPECT_EQ(degraded.consecutive_failures, 1u);
  EXPECT_EQ(degraded.last_error_kind, ErrorKind::Timeout);
  EXPECT_EQ(degraded.last_error_text, "no reply 1");
  EXPECT_EQ(degraded.ts, TimePoint{} + 7s);

  const DeviceHealthChanged& faulted = events.seen[1];
  EXPECT_EQ(faulted.from, DeviceState::Degraded);
  EXPECT_EQ(faulted.to, DeviceState::Faulted);
  EXPECT_EQ(faulted.consecutive_failures, 3u);
  EXPECT_EQ(faulted.last_error_text, "no reply 3");
}

TEST(Plain, RecoveryPublishesOk) {
  SignalBus bus;
  HealthEvents events(bus);
  Plain d("ig1", DeviceOptions{nullptr, 2});
  d.set_health_bus(&bus, "gauges");
  (void)d.observe(Result<void>{});  // Unknown -> Ok
  (void)d.observe(Result<void>{});  // no change
  (void)d.observe(failure(1));
  (void)d.observe(failure(2));
  (void)d.observe(Result<void>{});
  (void)d.observe(Result<void>(fail(ErrorKind::Cancelled, "stop")));  // not a failure: no change

  ASSERT_EQ(events.seen.size(), 4u);
  EXPECT_EQ(events.seen[0].from, DeviceState::Unknown);
  EXPECT_EQ(events.seen[0].to, DeviceState::Ok);
  EXPECT_FALSE(events.seen[0].last_error_kind.has_value());
  EXPECT_EQ(events.seen[1].to, DeviceState::Degraded);
  EXPECT_EQ(events.seen[2].to, DeviceState::Faulted);
  const DeviceHealthChanged& ok = events.seen[3];
  EXPECT_EQ(ok.from, DeviceState::Faulted);
  EXPECT_EQ(ok.to, DeviceState::Ok);
  EXPECT_EQ(ok.consecutive_failures, 0u);
  EXPECT_EQ(ok.last_error_kind, ErrorKind::Timeout);  // kept after recovery, as health() keeps it
}

// Threads failing and succeeding at once: what the bus delivers is one
// chain of changes, each from where the last one left the device, ending in
// the state the device is in.
TEST(Plain, ConcurrentChangesFormOneChain) {
  SignalBus bus;
  std::mutex seen_mutex;
  std::vector<DeviceHealthChanged> seen;
  const auto sub = bus.subscribe<DeviceHealthChanged>([&](const DeviceHealthChanged& e) {
    const std::lock_guard lock(seen_mutex);
    seen.push_back(e);
  });
  Plain d("ig1", DeviceOptions{nullptr, 3});
  d.set_health_bus(&bus, "gauges");
  {
    constexpr int kThreads = 8;
    std::vector<std::jthread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&d, t] {
        for (int i = 0; i < 2000; ++i) {
          // Mostly failures, so that every threshold is crossed again and again.
          if ((i + t) % 5 == 0) {
            (void)d.observe(Result<void>{});
          } else {
            (void)d.observe(failure(i));
          }
        }
      });
    }
  }
  const std::lock_guard lock(seen_mutex);
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.front().from, DeviceState::Unknown);
  for (std::size_t i = 1; i < seen.size(); ++i) {
    ASSERT_EQ(seen[i].from, seen[i - 1].to) << "event " << i << " of " << seen.size();
    ASSERT_NE(seen[i].from, seen[i].to);
  }
  EXPECT_EQ(seen.back().to, d.health().state);
}

// The error's text is carried, cut: a driver may put a whole reply in it.
TEST(Plain, ALongErrorTextIsCut) {
  SignalBus bus;
  HealthEvents events(bus);
  Plain d("ig1", DeviceOptions{nullptr, 3});
  d.set_health_bus(&bus, "gauges");
  (void)d.observe(Result<void>(fail(ErrorKind::Protocol, std::string(5000, 'x'))));
  ASSERT_EQ(events.seen.size(), 1u);
  EXPECT_LE(events.seen[0].last_error_text.size(), 515u);
  EXPECT_TRUE(events.seen[0].last_error_text.ends_with("\xE2\x80\xA6"));
}

TEST(Plain, NoBusPublishesNothing) {
  SignalBus bus;
  HealthEvents events(bus);
  Plain d("ig1", DeviceOptions{nullptr, 1});
  (void)d.observe(failure(1));
  (void)d.observe(Result<void>{});
  d.set_health_bus(&bus, "gauges");
  d.set_health_bus(nullptr);
  (void)d.observe(failure(2));
  EXPECT_TRUE(events.seen.empty());
  EXPECT_EQ(d.health().state, DeviceState::Faulted);
}

// ---- the log ---------------------------------------------------------------

TEST(DeviceLog, CallOkIsDebug) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  ManualClock clock;
  Plain d("ig1", DeviceOptions{&clock, 3});
  auto r = d.call<double>(
      "read_pressure",
      [&](Resend) -> Result<double> {
        clock.advance(std::chrono::milliseconds(12));
        return 1.5;
      },
      Repeat::Safe);
  ASSERT_TRUE(r);
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[debug] device.ig1: read_pressure ok in 12 ms")) << ::testing::PrintToString(lines);
}

TEST(DeviceLog, FailureIsWarn) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  log.hub().set_levels(LogLevel::Warn, {});
  Plain d("ig1");
  auto r = d.call<double>(
      "read_pressure", [](Resend) -> Result<double> { return fail(ErrorKind::Timeout, "no reply"); },
      Repeat::Safe);
  ASSERT_FALSE(r);
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[warn] device.ig1: read_pressure failed: timeout: no reply"))
      << ::testing::PrintToString(lines);
}

TEST(DeviceLog, PollingIsTrace) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Plain d("ig1");
  const auto read = [&] {
    const LogContext::Polling polling;
    auto r = d.call<double>("read_pressure", [](Resend) -> Result<double> { return 1.5; }, Repeat::Safe);
    ASSERT_TRUE(r);
  };
  read();  // default level debug: nothing
  for (const auto& line : log.lines()) EXPECT_EQ(line.find("device.ig1"), std::string::npos) << line;

  log.hub().set_levels(LogLevel::Trace, {});
  read();
  const auto lines = log.lines();
  EXPECT_TRUE(std::ranges::any_of(
      lines, [](const std::string& l) { return l.starts_with("[trace] device.ig1: read_pressure ok in "); }))
      << ::testing::PrintToString(lines);
}

namespace {

Result<double> failing_read(Plain& d, ErrorKind kind) {
  return d.call<double>(
      "read_pressure", [kind](Resend) -> Result<double> { return fail(kind, "no reply"); }, Repeat::Safe);
}

}  // namespace

TEST(DeviceLog, CancelledIsDebugNotWarn) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Plain d("ig1");
  ASSERT_FALSE(failing_read(d, ErrorKind::Cancelled));
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[debug] device.ig1: read_pressure failed: cancelled: no reply"))
      << ::testing::PrintToString(lines);
}

TEST(DeviceLog, WhilePollingOnlyTheFirstFailureOfAStreakWarns) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  log.hub().set_levels(LogLevel::Trace, {});
  Plain d("ig1");
  {
    const LogContext::Polling polling;
    ASSERT_FALSE(failing_read(d, ErrorKind::Timeout));
    ASSERT_FALSE(failing_read(d, ErrorKind::Timeout));
  }
  const auto lines = log.lines();
  const std::vector<std::string> wanted{"[warn] device.ig1: read_pressure failed: timeout: no reply",
                                        "[trace] device.ig1: read_pressure failed: timeout: no reply"};
  std::vector<std::string> got;
  for (const auto& l : lines)
    if (l.find("device.ig1: read_pressure failed") != std::string::npos) got.push_back(l);
  EXPECT_EQ(got, wanted);
}

TEST(DeviceLog, OutsidePollingARepeatedFailureStillWarns) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  Plain d("ig1");
  ASSERT_FALSE(failing_read(d, ErrorKind::Timeout));
  ASSERT_FALSE(failing_read(d, ErrorKind::Timeout));
  const auto lines = log.lines();
  EXPECT_EQ(std::ranges::count(lines, std::string("[warn] device.ig1: read_pressure failed: timeout: no reply")), 2)
      << ::testing::PrintToString(lines);
}

TEST(DeviceLog, RelayWritesATraceRecord) {
  nobion::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  log.hub().set_levels(LogLevel::Trace, {});
  ManualClock clock;
  Plain d("ig1", DeviceOptions{&clock, 3});
  auto r = d.relay<double>(
      "query",
      [&](Resend) -> Result<double> {
        clock.advance(std::chrono::milliseconds(3));
        return 1.5;
      },
      Repeat::Safe);
  ASSERT_TRUE(r);
  const auto lines = log.lines();
  EXPECT_TRUE(has_line(lines, "[trace] device.ig1: query ok in 3 ms")) << ::testing::PrintToString(lines);
}
