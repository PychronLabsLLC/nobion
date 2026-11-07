#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/signal_bus.hpp"
#include "temp_path.hpp"

// Spec 2026-11-06-field-diagnostics, 5.3: what writes out the flight recorder.

namespace fs = std::filesystem;
using namespace nobion;

namespace {

class TempDir {
 public:
  TempDir() : path_(nobion::testing::unique_temp_path("nobion_flight_triggers_")) { fs::create_directories(path_); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
};

std::shared_ptr<LogHub> make_hub(const fs::path& dir, const Clock& clock) {
  config::LoggingConfig cfg;
  cfg.dir = dir;
  auto hub = LogHub::create(cfg, clock);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

std::vector<fs::path> dumps_in(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir / "flight", ec), end; !ec && it != end; it.increment(ec)) out.push_back(it->path());
  std::sort(out.begin(), out.end());
  return out;
}

Alarm alarm_of(std::string source, AlarmSeverity severity) {
  Alarm a;
  a.source = std::move(source);
  a.severity = severity;
  a.message = "test";
  return a;
}

// A dump that records its reason and, until opened, holds the worker inside it.
class Latch {
 public:
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  Result<fs::path> dump(std::string_view reason) {
    std::unique_lock lock(mutex_);
    reasons_.emplace_back(reason);
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
    return fs::path(reason);
  }
  // Waits, at most `limit`, until `n` dumps have started.
  bool await_dumps(std::size_t n, std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, limit, [&] { return reasons_.size() >= n; });
  }
  std::vector<std::string> reasons() {
    const std::lock_guard lock(mutex_);
    return reasons_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
  std::vector<std::string> reasons_;
};

}  // namespace

TEST(FlightRecorderTriggers, ACriticalAlarmDumps) {
  TempDir tmp;
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(tmp.path(), clock);
  ASSERT_TRUE(hub);
  {
    const FlightRecorderTriggers triggers(*hub, bus);
    bus.publish(alarm_of("ig1", AlarmSeverity::Critical));
  }  // the destructor finishes what was asked before it returns
  const auto dumps = dumps_in(tmp.path());
  ASSERT_EQ(dumps.size(), 1u);
  EXPECT_TRUE(dumps[0].filename().string().ends_with("-alarm-ig1.log")) << dumps[0];
}

TEST(FlightRecorderTriggers, AWarningAlarmDoesNot) {
  TempDir tmp;
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(tmp.path(), clock);
  ASSERT_TRUE(hub);
  {
    const FlightRecorderTriggers triggers(*hub, bus);
    bus.publish(alarm_of("ig1", AlarmSeverity::Warning));
    bus.publish(alarm_of("ig1", AlarmSeverity::Info));
  }
  EXPECT_TRUE(dumps_in(tmp.path()).empty());
}

// The bus handler only queues: a dump that does not return holds up nothing
// that publishes.
TEST(FlightRecorderTriggers, RequestNeverBlocksThePublisher) {
  const SteadyClock clock;
  SignalBus bus;
  config::LoggingConfig cfg;
  cfg.dir = "";
  auto hub = LogHub::create(cfg, clock);
  ASSERT_TRUE(hub);
  Latch latch;
  std::optional<FlightRecorderTriggers> triggers;
  triggers.emplace(**hub, bus, [&latch](std::string_view reason) { return latch.dump(reason); });

  bus.publish(alarm_of("first", AlarmSeverity::Critical));
  ASSERT_TRUE(latch.await_dumps(1, std::chrono::seconds(5)));  // the worker is inside the dump, held

  const auto before = std::chrono::steady_clock::now();
  bus.publish(alarm_of("second", AlarmSeverity::Critical));
  const auto took = std::chrono::steady_clock::now() - before;
  EXPECT_LT(took, std::chrono::seconds(5));
  EXPECT_EQ(latch.reasons().size(), 1u);  // still held: the second waits its turn

  latch.open();
  triggers.reset();  // joins the worker after the queued request
  EXPECT_EQ(latch.reasons(), (std::vector<std::string>{"alarm-first", "alarm-second"}));
}

// Review Focus 5: a storm of critical alarms leaves one dump, the first's.
TEST(FlightRecorderTriggers, AStormKeepsItsFirstDump) {
  TempDir tmp;
  const SteadyClock clock;
  SignalBus bus;
  auto hub = make_hub(tmp.path(), clock);
  ASSERT_TRUE(hub);
  // All 30 inside one second of the limits' time.
  const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  int calls = 0;
  std::mutex calls_mutex;
  hub->set_recorder_limit_clock_for_testing([&] {
    const std::lock_guard lock(calls_mutex);
    return start + std::chrono::milliseconds(30 * calls++);
  });
  {
    const FlightRecorderTriggers triggers(*hub, bus);
    for (int i = 0; i < 30; ++i) bus.publish(alarm_of("s" + std::to_string(i), AlarmSeverity::Critical));
  }
  const auto dumps = dumps_in(tmp.path());
  ASSERT_EQ(dumps.size(), 1u);
  EXPECT_TRUE(dumps[0].filename().string().ends_with("-alarm-s0.log")) << dumps[0];
}
