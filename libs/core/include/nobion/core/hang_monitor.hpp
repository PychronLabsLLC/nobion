#pragma once

// Reports a thread that has gone silent (spec 2026-11-06-field-diagnostics,
// 6): the UI thread, the scheduler, a transport call that does not return.

#include <chrono>
#include <memory>
#include <string>
#include <thread>


namespace nobion {

class Clock;
class LogHub;
class Logger;
class SignalBus;
struct IDumpRequester;

namespace detail {
struct HangState;
struct HangEntry;
}  // namespace detail

// Outside HangMonitor so that `Options options = {}` can default it there.
struct HangMonitorOptions {
  std::chrono::milliseconds poll{1000};  // how often beats are looked at
  // How long detach() waits for a report in progress before it gives up.
  std::chrono::milliseconds detach_wait{5000};
};

// One thread on real time, on purpose: it watches threads that may hold the
// virtual clock still, so it can never be one of the clock's participants
// (virtual clock spec 4.4). It is not Scheduler::watchdog either: that runs on
// the injected clock and the scheduler pool, which are among what hangs.
//
// A beat is pulsed by the thread it watches. An armed beat silent past its
// limit is reported, in this order: an `error` record `hang: <name> silent for
// <s> s` on logger `hang` into the flight recorder's ring only (it waits on
// nothing); `recorder.request("hang-<name>")`; the same record to the file,
// stderr and bus (Logger::log_unrecorded: an `error` record waits for the
// disk, which may be what hangs); then a warning `Alarm{source = "hang"}`
// published from the monitor thread (last: a hung bus handler may block the
// publish). Once per episode: the next report of that beat comes only after a
// pulse.
//
// Late attach: a monitor can be made before what it reports to (an app makes
// it before the line whose transports it watches, and whose bus it reports
// on), and attach()ed once those exist. Its thread starts at the first
// attach(): a monitor never attached runs none. Unattached it reports
// nothing, and a beat already silent is reported once attached. A Beat may
// outlive the monitor: its pulses then go nowhere.
class HangMonitor {
 public:
  using Options = HangMonitorOptions;

  // Movable; the destructor unregisters it.
  class Beat {
   public:
    Beat() = default;
    ~Beat();
    Beat(const Beat&) = delete;
    Beat& operator=(const Beat&) = delete;
    Beat(Beat&& other) noexcept = default;
    Beat& operator=(Beat&& other) noexcept;

    // Lock-free: atomic stores of steady_clock::now() and of the limit. Arms
    // a disarmed beat. Without `limit`, the one the beat was made with.
    void pulse() noexcept;
    // This episode's limit only (a call with a longer timeout of its own);
    // the next pulse() goes back to the beat's.
    void pulse(std::chrono::milliseconds limit) noexcept;
    // Not watched until the next pulse.
    void disarm() noexcept;

   private:
    friend class HangMonitor;
    Beat(std::shared_ptr<detail::HangState> state, std::shared_ptr<detail::HangEntry> entry) noexcept;
    void reset() noexcept;

    std::shared_ptr<detail::HangState> state_;
    std::shared_ptr<detail::HangEntry> entry_;
  };

  // Unattached: hands out beats, runs no thread and reports nothing until
  // attach().
  explicit HangMonitor(Options options = {});
  // Made and attached at once.
  HangMonitor(LogHub& log, IDumpRequester& recorder, SignalBus& bus, Options options = {});
  ~HangMonitor();  // stops and joins the monitor thread
  HangMonitor(const HangMonitor&) = delete;
  HangMonitor& operator=(const HangMonitor&) = delete;
  HangMonitor(HangMonitor&&) = delete;
  HangMonitor& operator=(HangMonitor&&) = delete;

  // Reports go to these from now on (a second attach replaces the first).
  // `clock`, when given, stamps the Alarm (`ts = clock->now()`); otherwise
  // its ts is left unset. All must stay alive until detach() or the
  // monitor's destruction.
  void attach(LogHub& log, IDumpRequester& recorder, SignalBus& bus, const Clock* clock = nullptr);
  // No more reports. Returns once a report in progress has finished, or
  // after Options::detach_wait, when it logs a `warn` that it gave up: what
  // was attached may then be destroyed. A report it gave up on makes no call
  // to the recorder or the bus after the one it is stuck in returns (one
  // stuck in either still is: the bound trades that for a shutdown that
  // ends). Idempotent.
  void detach();

  // Detaches in its destructor. Declared after what was attached, so that it
  // goes before them on every way out.
  class DetachOnExit {
   public:
    explicit DetachOnExit(HangMonitor& monitor) noexcept : monitor_(&monitor) {}
    ~DetachOnExit() { monitor_->detach(); }
    DetachOnExit(const DetachOnExit&) = delete;
    DetachOnExit& operator=(const DetachOnExit&) = delete;
    DetachOnExit(DetachOnExit&&) = delete;
    DetachOnExit& operator=(DetachOnExit&&) = delete;

   private:
    HangMonitor* monitor_;
  };

  // `armed`: as if pulsed now; otherwise not watched until the first pulse.
  [[nodiscard]] Beat beat(std::string name, std::chrono::milliseconds limit, bool armed = true);

 private:
  // What one report goes to, copied under the state's mutex.
  struct Target;
  void run();
  void report(const detail::HangEntry& entry, std::chrono::nanoseconds silent, const Target& target);
  [[nodiscard]] bool still_attached(const Target& target) const;

  Options options_;
  std::shared_ptr<detail::HangState> state_;
  std::thread thread_;  // started by the first attach()
};

}  // namespace nobion
