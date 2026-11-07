#include "nobion/core/flight_recorder_triggers.hpp"

#include <utility>

#include "nobion/core/events.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/signal_stack.hpp"

namespace nobion {

FlightRecorderTriggers::FlightRecorderTriggers(LogHub& hub, SignalBus& bus, DumpFn dump) : dump_(std::move(dump)) {
  if (!dump_) dump_ = [&hub](std::string_view reason) { return hub.dump_recorder(reason); };
  worker_ = std::thread([this] { work(); });
  alarm_ = bus.subscribe<Alarm>([this](const Alarm& a) {
    if (a.severity == AlarmSeverity::Critical) request("alarm-" + a.source);
  });
}

FlightRecorderTriggers::~FlightRecorderTriggers() {
  alarm_.reset();
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

void FlightRecorderTriggers::request(std::string reason) {
  {
    const std::lock_guard lock(mutex_);
    if (stopping_ || pending_.size() >= kMaxPending) return;
    pending_.push_back(std::move(reason));
  }
  cv_.notify_one();
}

void FlightRecorderTriggers::work() {
  install_signal_stack();
  const LogContext::Thread tag("flight");
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
    if (pending_.empty()) return;  // stopping, and nothing left to write
    const std::string reason = std::move(pending_.front());
    pending_.pop_front();
    lock.unlock();
    // A failure is the hub's to log (Io), or means the recorder is off (Config).
    (void)dump_(reason);
    lock.lock();
  }
}

}  // namespace nobion
