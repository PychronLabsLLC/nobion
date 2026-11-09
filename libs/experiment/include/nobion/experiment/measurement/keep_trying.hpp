#pragma once

// A call that must get through while the line is in a state not safe to
// leave it in (executor hardening, spec 4.4 and 4.7): a valve or detector
// call during a measurement, a step of the safe state. It is repeated through
// transient failures until it succeeds or its window passes.

#include <chrono>
#include <functional>
#include <string>

#include "nobion/core/clock.hpp"
#include "nobion/core/error.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion::experiment::measurement {

// Spec 4.4: the critical window, and when an alarm first says so.
inline constexpr nobion::Duration kCriticalWindow = std::chrono::minutes(10);
inline constexpr nobion::Duration kCriticalAlarmAfter = std::chrono::seconds(20);

struct KeepTrying {
  const Clock& clock;
  SignalBus* bus = nullptr;
  std::string source;  // the Alarm's source
  // Waits on the clock; false: stop trying (the run was cancelled).
  std::function<bool(nobion::Duration)> sleep;
  // Said with each alarm (a run note, a log line); may be empty.
  std::function<void(const std::string&)> say;
  nobion::Duration window = kCriticalWindow;
  nobion::Duration alarm_after = kCriticalAlarmAfter;
};

// `op` until it succeeds. A transient failure (Timeout, Io, NotConnected) is
// tried again after a backoff of 1 s doubling to 30 s, for `window`; a
// warning Alarm goes out at `alarm_after`, a critical one when it gives up.
// A definite error gives up at once, with a critical Alarm. `what` names the
// call in the alarms ("close inlet B").
[[nodiscard]] Result<void> keep_trying(const KeepTrying& how, const std::string& what,
                                       const std::function<Result<void>()>& op);

}  // namespace nobion::experiment::measurement
