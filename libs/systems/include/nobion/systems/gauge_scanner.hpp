#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/config/system_config.hpp"
#include "nobion/core/scheduler.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/devices/capabilities.hpp"
#include "nobion/devices/channel_gauge.hpp"

namespace nobion {

// Registers configured gauges with the Scheduler, publishes PressureSample per
// successful read, and evaluates alarm_high / alarm_low. An Alarm is published
// on the transition into an alarm condition (not on every sample) and again
// after the gauge has returned to range and re-entered. A failed read
// publishes one Warning Alarm until the next successful read.
//
// Borrowed: scheduler, bus, clock, and the drivers must outlive the scanner.
class GaugeScanner {
 public:
  using Reader = std::function<Result<double>()>;

  GaugeScanner(Scheduler& scheduler, SignalBus& bus, const Clock& clock);
  ~GaugeScanner();
  GaugeScanner(const GaugeScanner&) = delete;
  GaugeScanner& operator=(const GaugeScanner&) = delete;

  // Single-channel gauge driver.
  Result<void> add_gauge(const config::GaugeConfig& gauge, IPressureGauge& driver, Duration interval);
  // Multi-channel controller; reads gauge.channel.
  Result<void> add_gauge(const config::GaugeConfig& gauge, IChannelPressureGauge& driver, Duration interval);
  // Generic reader.
  Result<void> add_gauge(const config::GaugeConfig& gauge, Reader reader, Duration interval);

  // Cancels all scheduled scans. Idempotent; called by the destructor.
  void stop();

  std::size_t gauge_count() const;

 private:
  struct State;

  Scheduler& scheduler_;
  SignalBus& bus_;
  const Clock& clock_;
  mutable std::mutex mutex_;
  std::vector<JobId> jobs_;
  std::vector<std::shared_ptr<State>> states_;
};

}  // namespace nobion
