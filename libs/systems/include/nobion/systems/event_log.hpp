#pragma once

// Bus tap for the events libs/systems publishes (jobs, peak center,
// acquisition, spectrometer, scan service): each one record on logger
// "trace.<EventName>", as nobion::EventRecorder does for the core events
// (nobion/core/event_recorder.hpp, which says how).

#include <span>
#include <vector>

#include "nobion/core/event_recorder.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion::systems {

class SystemsEventRecorder {
 public:
  explicit SystemsEventRecorder(SignalBus& bus);

 private:
  std::vector<SignalBus::Subscription> subscriptions_;
};

[[nodiscard]] std::span<const EventClass> systems_event_classes() noexcept;

}  // namespace nobion::systems
