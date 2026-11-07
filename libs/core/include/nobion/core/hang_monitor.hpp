#pragma once

// Reports a thread that has gone silent (spec 2026-11-06-field-diagnostics,
// 6): the UI thread, the scheduler, a transport call that does not return.

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "nobion/core/logger.hpp"

namespace nobion {

class LogHub;
class SignalBus;
struct IDumpRequester;

namespace detail {
struct HangState;
struct HangEntry;
}  // namespace detail

// Outside HangMonitor so that `Options options = {}` can default it there.
struct HangMonitorOptions {
  std::chrono::milliseconds poll{1000};  // how often beats are looked at
};

// One thread on real time, on purpose: it watches threads that may hold the
// virtual clock still, so it can never be one of the clock's participants
// (virtual clock spec 4.4). It is not Scheduler::watchdog either: that runs on
// the injected clock and the scheduler pool, which are among what hangs.
//
// A beat is pulsed by the thread it watches. An armed beat silent past its
// limit is reported, in this order: an `error` record `hang: <name> silent for
// <s> s` on logger `hang`; `recorder.request("hang-<name>")`; then a warning
// `Alarm{source = "hang"}` published from the monitor thread (last: a hung bus
// handler may block the publish). Once per episode: the next report of that
// beat comes only after a pulse.
//
// `log`, `recorder` and `bus` must outlive the monitor. A Beat may outlive it:
// its pulses then go nowhere.
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

    // Lock-free: one atomic store of steady_clock::now(). Arms a disarmed beat.
    void pulse() noexcept;
    // Not watched until the next pulse.
    void disarm() noexcept;

   private:
    friend class HangMonitor;
    Beat(std::shared_ptr<detail::HangState> state, std::shared_ptr<detail::HangEntry> entry) noexcept;
    void reset() noexcept;

    std::shared_ptr<detail::HangState> state_;
    std::shared_ptr<detail::HangEntry> entry_;
  };

  HangMonitor(LogHub& log, IDumpRequester& recorder, SignalBus& bus, Options options = {});
  ~HangMonitor();  // stops and joins the monitor thread
  HangMonitor(const HangMonitor&) = delete;
  HangMonitor& operator=(const HangMonitor&) = delete;
  HangMonitor(HangMonitor&&) = delete;
  HangMonitor& operator=(HangMonitor&&) = delete;

  // Starts armed, as if pulsed now.
  [[nodiscard]] Beat beat(std::string name, std::chrono::milliseconds limit);

 private:
  void run();
  void report(detail::HangEntry& entry, std::chrono::steady_clock::duration silent);

  Logger logger_;
  IDumpRequester& recorder_;
  SignalBus& bus_;
  Options options_;
  std::shared_ptr<detail::HangState> state_;
  std::thread thread_;  // last: started once everything it reads is made
};

}  // namespace nobion
