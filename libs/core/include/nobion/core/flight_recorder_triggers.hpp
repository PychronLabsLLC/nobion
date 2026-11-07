#pragma once

// What writes out the flight recorder (spec 2026-11-06-field-diagnostics,
// 5.3): a critical alarm, a device going to Faulted, and whatever else calls request() (a failed run,
// an unsafe queue end: libs/experiment flight_recorder_wiring.hpp).

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "nobion/core/error.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/signal_bus.hpp"

namespace nobion {

class LogHub;

// Asks for a dump of the flight recorder with `reason` (the dump's name and
// first line say it). Never blocks, never does file I/O on the caller's thread.
struct IDumpRequester {
  virtual ~IDumpRequester() = default;
  virtual void request(std::string reason) = 0;
};

// Owns one worker thread that makes the dumps (`hub.dump_recorder`, which
// keeps its own limits), so that no bus handler writes a file on the thread
// that published. The worker is on real time and never a Clock participant
// (virtual clock spec 4.4): it waits only on this object's own queue and on
// the disk.
//
// Subscribes `Alarm`: one of severity Critical asks for `alarm-<source>`.
// Subscribes `DeviceHealthChanged`: one to Faulted asks for
// `faulted-<device>` (spec section 7).
//
// At most kMaxPending requests wait; one past that is dropped (a storm keeps
// its first dump: the hub's limits turn the rest away anyway). The
// destructor unsubscribes, makes the dumps already asked for, then joins the
// worker: a request made just before a program ends is still written. `hub`
// and `bus` must outlive this object. Whatever publishes an `Alarm` or a
// `DeviceHealthChanged` on `bus` from another thread is stopped before this
// object is destroyed: the unsubscribe does not wait for a handler already
// running there, which would then call request() on a destroyed object.
//
// A dump the hub refuses (a limit, the recorder off) or fails to write is
// logged at `debug` on logger `flight` (the hub logs a failed write itself).
class FlightRecorderTriggers final : public IDumpRequester {
 public:
  using DumpFn = std::function<Result<std::filesystem::path>(std::string_view)>;
  static constexpr std::size_t kMaxPending = 32;

  // `dump` empty: hub.dump_recorder. Tests give one of their own.
  FlightRecorderTriggers(LogHub& hub, SignalBus& bus, DumpFn dump = {});
  ~FlightRecorderTriggers() override;
  FlightRecorderTriggers(const FlightRecorderTriggers&) = delete;
  FlightRecorderTriggers& operator=(const FlightRecorderTriggers&) = delete;
  FlightRecorderTriggers(FlightRecorderTriggers&&) = delete;
  FlightRecorderTriggers& operator=(FlightRecorderTriggers&&) = delete;

  // Queues a dump. Thread-safe; holds a mutex only to push onto the queue.
  void request(std::string reason) override;

 private:
  void work();

  DumpFn dump_;
  Logger log_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> pending_;
  bool stopping_ = false;
  std::thread worker_;
  // After the worker: made once it runs, reset first in the destructor.
  SignalBus::Subscription alarm_;
  SignalBus::Subscription device_;
};

}  // namespace nobion
