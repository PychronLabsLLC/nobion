#pragma once

// The experiment's events that write out the flight recorder (spec
// 2026-11-06-field-diagnostics, 5.3): a run that ends Failed asks for
// `run-failed-<run id>`, a queue that ends with code `unsafe` (safe state not
// reached) for `unsafe`.

#include <vector>

#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion::experiment {

// The handlers only call `requester.request`, which never blocks. `requester`
// must outlive the subscriptions.
[[nodiscard]] std::vector<SignalBus::Subscription> wire_flight_recorder(SignalBus& bus, IDumpRequester& requester);

}  // namespace nobion::experiment
