#pragma once

// Bus tap for the events libs/experiment publishes (executor, run, session,
// measurement engine, collector, persister, notifier): each one record on
// logger "trace.<EventName>", as nobion::EventRecorder does for the core
// events (nobion/core/event_recorder.hpp, which says how).

#include <span>
#include <vector>

#include "nobion/core/event_recorder.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion::experiment {

class ExperimentEventRecorder {
 public:
  explicit ExperimentEventRecorder(SignalBus& bus);

 private:
  std::vector<SignalBus::Subscription> subscriptions_;
};

[[nodiscard]] std::span<const EventClass> experiment_event_classes() noexcept;

}  // namespace nobion::experiment
