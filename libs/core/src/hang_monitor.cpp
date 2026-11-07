#include "nobion/core/hang_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/core/signal_stack.hpp"

namespace nobion {

namespace {

// steady_clock::now() in nanoseconds, never 0 (0 is "disarmed").
std::int64_t steady_stamp() noexcept {
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
  return std::max<std::int64_t>(1, ns.count());
}

// A pulse's two stores, ordered against the monitor, which loads the limit
// and then the last pulse: whichever pair it sees, the limit it uses belongs
// to the pulse it measures from or to an earlier, older one with a longer
// limit, never a short limit with an old stamp (a false hang right after a
// long call). Shortening: the stamp first; lengthening: the limit first.
void store_pulse(std::atomic<std::int64_t>& limit, std::atomic<std::int64_t>& last, std::int64_t new_limit) noexcept {
  if (new_limit < limit.load()) {  // only the watched thread writes `limit`
    last.store(steady_stamp());
    limit.store(new_limit);
  } else {
    limit.store(new_limit);
    last.store(steady_stamp());
  }
}

}  // namespace

namespace detail {

struct HangEntry {
  HangEntry(std::string n, std::chrono::nanoseconds l) : name(std::move(n)), base_limit(l), limit(l.count()) {}

  const std::string name;
  const std::chrono::nanoseconds base_limit;
  // What the watched thread writes: the limit of this episode (ns), and the
  // last pulse (steady_stamp), or 0 while disarmed.
  std::atomic<std::int64_t> limit;
  std::atomic<std::int64_t> last{0};
  // The pulse whose silence was last reported. Monitor thread only.
  std::int64_t reported = 0;
};

struct HangState {
  std::mutex mutex;  // guards all below; never held across a report
  std::condition_variable cv;    // stopping, for the monitor thread
  std::condition_variable idle;  // reporting went false, for detach()
  bool stopping = false;
  std::vector<std::shared_ptr<HangEntry>> entries;
  // What reports go to; recorder is null while unattached.
  std::optional<Logger> logger;
  IDumpRequester* recorder = nullptr;
  SignalBus* bus = nullptr;
  const Clock* clock = nullptr;
  bool reporting = false;     // the monitor thread is using the four above
  std::uint64_t detaches = 0;  // bumped by every detach(): a report started before one stops using them
  std::thread::id monitor;    // the monitor thread, once attach() has started it
};

}  // namespace detail

struct HangMonitor::Target {
  std::optional<Logger> logger;
  IDumpRequester* recorder = nullptr;
  SignalBus* bus = nullptr;
  const Clock* clock = nullptr;
  std::uint64_t detaches = 0;  // HangState::detaches when it was copied
};

HangMonitor::Beat::Beat(std::shared_ptr<detail::HangState> state, std::shared_ptr<detail::HangEntry> entry) noexcept
    : state_(std::move(state)), entry_(std::move(entry)) {}

HangMonitor::Beat::~Beat() { reset(); }

HangMonitor::Beat& HangMonitor::Beat::operator=(Beat&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    entry_ = std::move(other.entry_);
  }
  return *this;
}

void HangMonitor::Beat::pulse() noexcept {
  if (!entry_) return;
  store_pulse(entry_->limit, entry_->last, entry_->base_limit.count());
}

void HangMonitor::Beat::pulse(std::chrono::milliseconds limit) noexcept {
  if (!entry_) return;
  store_pulse(entry_->limit, entry_->last, std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count());
}

void HangMonitor::Beat::disarm() noexcept {
  if (entry_) entry_->last.store(0);
}

void HangMonitor::Beat::reset() noexcept {
  if (state_ && entry_) {
    const std::lock_guard lock(state_->mutex);
    std::erase(state_->entries, entry_);
  }
  state_.reset();
  entry_.reset();
}

HangMonitor::HangMonitor(Options options) : options_(options), state_(std::make_shared<detail::HangState>()) {
  options_.poll = std::max(options_.poll, std::chrono::milliseconds(1));
  options_.detach_wait = std::max(options_.detach_wait, std::chrono::milliseconds(0));
}

HangMonitor::HangMonitor(LogHub& log, IDumpRequester& recorder, SignalBus& bus, Options options)
    : HangMonitor(options) {
  attach(log, recorder, bus);
}

HangMonitor::~HangMonitor() {
  if (!thread_.joinable()) return;  // never attached: no thread was started
  {
    const std::lock_guard lock(state_->mutex);
    state_->stopping = true;
  }
  state_->cv.notify_all();
  thread_.join();
}

void HangMonitor::attach(LogHub& log, IDumpRequester& recorder, SignalBus& bus, const Clock* clock) {
  Logger logger = log.logger("hang");
  const std::lock_guard lock(state_->mutex);
  state_->logger.emplace(std::move(logger));
  state_->recorder = &recorder;
  state_->bus = &bus;
  state_->clock = clock;
  // Started here, not in the constructor: an app that never has anything to
  // report to never runs it. Under the mutex: the thread's first look at the
  // state waits for it, and detach() reads `monitor` under it.
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { run(); });
    state_->monitor = thread_.get_id();
  }
}

void HangMonitor::detach() {
  std::optional<Logger> logger;
  bool gave_up = false;
  {
    std::unique_lock lock(state_->mutex);
    state_->recorder = nullptr;
    state_->bus = nullptr;
    state_->clock = nullptr;
    ++state_->detaches;
    // From inside a report (a bus handler on the monitor thread) there is
    // nothing to wait for: the report goes on with its own copies.
    if (std::this_thread::get_id() != state_->monitor) {
      gave_up = !state_->idle.wait_for(lock, options_.detach_wait, [this] { return !state_->reporting; });
    }
    logger = std::move(state_->logger);
    state_->logger.reset();
  }
  // The report still going is stuck in its log record or in a call to the
  // recorder; it makes no call to the recorder or the bus once that returns.
  if (gave_up && logger) {
    logger->warn("hang monitor: detach gave up waiting for a report in progress after " +
                 std::to_string(options_.detach_wait.count()) + " ms");
  }
}

HangMonitor::Beat HangMonitor::beat(std::string name, std::chrono::milliseconds limit, bool armed) {
  auto entry = std::make_shared<detail::HangEntry>(std::move(name), limit);
  if (armed) entry->last.store(steady_stamp());
  {
    const std::lock_guard lock(state_->mutex);
    state_->entries.push_back(entry);
  }
  return Beat(state_, std::move(entry));
}

// Real time on purpose (virtual clock spec 4.4): never a Clock participant,
// waits only on its own condition variable.
void HangMonitor::run() {
  install_signal_stack();
  const LogContext::Thread tag("hang");
  std::vector<std::shared_ptr<detail::HangEntry>> entries;
  for (;;) {
    {
      std::unique_lock lock(state_->mutex);
      state_->cv.wait_for(lock, options_.poll, [this] { return state_->stopping; });
      if (state_->stopping) return;
      // Unattached: nothing is looked at, so nothing is marked reported, and
      // a beat silent now is reported once attached.
      if (state_->recorder == nullptr) continue;
      entries = state_->entries;
    }
    for (const auto& entry : entries) {
      // The limit before the stamp: the order store_pulse() writes against.
      const std::int64_t limit = entry->limit.load();
      const std::int64_t last = entry->last.load();
      if (last == 0 || last == entry->reported) continue;
      const auto silent = std::chrono::nanoseconds(steady_stamp() - last);
      if (silent.count() <= limit) continue;
      Target target;
      {
        const std::lock_guard lock(state_->mutex);
        if (state_->recorder == nullptr) break;  // detached meanwhile: nothing marked
        target.logger = state_->logger;
        target.recorder = state_->recorder;
        target.bus = state_->bus;
        target.clock = state_->clock;
        target.detaches = state_->detaches;
        state_->reporting = true;
      }
      entry->reported = last;  // once per episode: the next report needs a pulse
      report(*entry, silent, target);
      {
        const std::lock_guard lock(state_->mutex);
        state_->reporting = false;
      }
      state_->idle.notify_all();
    }
    entries.clear();  // a beat let go is not kept alive here until the next poll
  }
}

bool HangMonitor::still_attached(const Target& target) const {
  const std::lock_guard lock(state_->mutex);
  return state_->detaches == target.detaches;
}

// The order is what a hung disk or bus leaves standing (spec 6): the record
// goes into the ring first, which waits on nothing, so the dump asked for
// next has it; then the dump; then the record to the file, which an `error`
// record waits for (a hung disk holds it there); the Alarm last, since a hung
// bus handler may block the publish. A detach() that gave up waiting for this
// report ends it at the next step that would use the recorder or the bus.
void HangMonitor::report(const detail::HangEntry& entry, std::chrono::nanoseconds silent, const Target& target) {
  std::ostringstream text;
  text << "hang: " << entry.name << " silent for " << std::fixed << std::setprecision(1)
       << std::chrono::duration<double>(silent).count() << " s";
  const std::string message = text.str();
  target.logger->record(LogLevel::Error, message);
  if (!still_attached(target)) return;
  target.recorder->request("hang-" + entry.name);
  target.logger->log_unrecorded(LogLevel::Error, message);
  if (!still_attached(target)) return;
  Alarm alarm;
  alarm.source = "hang";
  alarm.severity = AlarmSeverity::Warning;
  alarm.message = message;
  if (target.clock != nullptr) alarm.ts = target.clock->now();  // unset otherwise: the monitor has no clock of its own
  target.bus->publish(alarm);
}

}  // namespace nobion
