#include "nobion/devices/device.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "nobion/core/events.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/process_log.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion {
namespace {

// The level of a record that says an operation went through.
LogLevel ok_level() noexcept { return LogContext::polling() ? LogLevel::Trace : LogLevel::Debug; }

std::string took(std::string_view what, TimePoint started, TimePoint now) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
  std::string message(what);
  message.append(" ok in ").append(std::to_string(ms)).append(" ms");
  return message;
}

std::string failed(std::string_view what, const Error& error) {
  std::string message(what);
  message.append(" failed: ").append(to_string(error.kind)).append(": ").append(error.what);
  return message;
}

// The state `failures` in a row mean, for a device that has (or has never)
// had an operation go through.
DeviceState state_of(std::uint64_t failures, bool ever_ok, std::uint64_t fault_after) noexcept {
  if (failures == 0) return ever_ok ? DeviceState::Ok : DeviceState::Unknown;
  return failures >= fault_after ? DeviceState::Faulted : DeviceState::Degraded;
}

}  // namespace

Device::Device(std::string name, DeviceOptions options)
    : name_(std::move(name)),
      clock_(options.clock),
      fault_after_(options.fault_after),
      link_(options.link),
      log_(process_logger("device." + name_)) {}

DeviceHealth Device::health() const {
  DeviceHealth h;
  h.consecutive_failures = failures_.load();
  h.last_ok = TimePoint(Duration(last_ok_.load()));
  if (int e = last_error_.load(); e >= 0) h.last_error = static_cast<ErrorKind>(e);
  h.state = state_of(h.consecutive_failures, ever_ok_.load(), fault_after_);
  return h;
}

void Device::set_health_bus(SignalBus* bus, std::string transport) noexcept {
  health_bus_ = bus;
  transport_ = std::move(transport);
}

TimePoint Device::log_now() const { return clock_ ? clock_->now() : SteadyClock().now(); }

Device::Operation Device::begin(std::string_view what) const {
  if (!log_.enabled(ok_level())) return {what, std::nullopt};
  return {what, log_now()};
}

std::optional<TimePoint> Device::trace_start() const {
  if (!log_.enabled(LogLevel::Trace)) return std::nullopt;
  return log_now();
}

void Device::log_outcome(const Operation& op, const Error* error, bool failing) const {
  if (error == nullptr) {
    // No start: the record was off when the operation began.
    if (op.started && log_.enabled(ok_level())) log_.log(ok_level(), took(op.what, *op.started, log_now()));
    return;
  }
  // Not a device failure (a cancel), or a poll of a device already failing
  // (said at warn when the streak began): at the level of an ok.
  const LogLevel level = error->kind == ErrorKind::Cancelled || (failing && LogContext::polling())
                             ? ok_level()
                             : LogLevel::Warn;
  if (log_.enabled(level)) log_.log(level, failed(op.what, *error));
}

void Device::log_relay(std::string_view what, TimePoint started, const Error* error) const {
  if (!log_.enabled(LogLevel::Trace)) return;
  log_.trace(error == nullptr ? took(what, started, log_now()) : failed(what, *error));
}

void Device::record_ok() {
  const TimePoint at = log_now();
  last_ok_.store(at.time_since_epoch().count());
  const std::uint64_t before = failures_.exchange(0);
  const bool was_ok = ever_ok_.exchange(true);
  publish_change(state_of(before, was_ok, fault_after_), DeviceState::Ok, 0, nullptr);
}

void Device::record_failure(const Error& error) {
  if (error.kind == ErrorKind::Cancelled) return;
  last_error_.store(static_cast<int>(error.kind));
  const std::uint64_t before = failures_.fetch_add(1);
  const bool ok = ever_ok_.load();
  publish_change(state_of(before, ok, fault_after_), state_of(before + 1, ok, fault_after_), before + 1, &error);
}

void Device::publish_change(DeviceState from, DeviceState to, std::uint64_t failures, const Error* error) const {
  if (from == to || health_bus_ == nullptr) return;
  DeviceHealthChanged e;
  e.device = name_;
  e.transport = transport_;
  e.from = from;
  e.to = to;
  e.consecutive_failures = failures;
  if (error != nullptr) {
    e.last_error_kind = error->kind;
    e.last_error_text = error->what;
  } else if (const int kind = last_error_.load(); kind >= 0) {
    e.last_error_kind = static_cast<ErrorKind>(kind);
  }
  e.ts = log_now();
  health_bus_->publish(e);
}

}  // namespace nobion
