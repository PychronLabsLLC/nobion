#include "nobion/core/hang_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
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
  bool reporting = false;  // the monitor thread is using the four above
};

}  // namespace detail

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
  entry_->limit.store(entry_->base_limit.count());
  entry_->last.store(steady_stamp());
}

void HangMonitor::Beat::pulse(std::chrono::milliseconds limit) noexcept {
  if (!entry_) return;
  entry_->limit.store(std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count());
  entry_->last.store(steady_stamp());
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
  thread_ = std::thread([this] { run(); });
}

HangMonitor::HangMonitor(LogHub& log, IDumpRequester& recorder, SignalBus& bus, Options options)
    : HangMonitor(options) {
  attach(log, recorder, bus);
}

HangMonitor::~HangMonitor() {
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
}

void HangMonitor::detach() {
  std::unique_lock lock(state_->mutex);
  state_->recorder = nullptr;
  state_->bus = nullptr;
  state_->clock = nullptr;
  // From inside a report (a bus handler on the monitor thread) there is
  // nothing to wait for: the report goes on with its own copies.
  if (std::this_thread::get_id() != thread_.get_id()) state_->idle.wait(lock, [this] { return !state_->reporting; });
  state_->logger.reset();
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
      const std::int64_t last = entry->last.load();
      if (last == 0 || last == entry->reported) continue;
      const auto silent = std::chrono::nanoseconds(steady_stamp() - last);
      if (silent.count() <= entry->limit.load()) continue;
      std::optional<Logger> logger;
      IDumpRequester* recorder = nullptr;
      SignalBus* bus = nullptr;
      const Clock* clock = nullptr;
      {
        const std::lock_guard lock(state_->mutex);
        if (state_->recorder == nullptr) break;  // detached meanwhile: nothing marked
        logger = state_->logger;
        recorder = state_->recorder;
        bus = state_->bus;
        clock = state_->clock;
        state_->reporting = true;
      }
      entry->reported = last;  // once per episode: the next report needs a pulse
      report(*entry, silent, *logger, *recorder, *bus, clock);
      {
        const std::lock_guard lock(state_->mutex);
        state_->reporting = false;
      }
      state_->idle.notify_all();
    }
    entries.clear();  // a beat let go is not kept alive here until the next poll
  }
}

void HangMonitor::report(const detail::HangEntry& entry, std::chrono::nanoseconds silent, const Logger& logger,
                         IDumpRequester& recorder, SignalBus& bus, const Clock* clock) {
  std::ostringstream text;
  text << "hang: " << entry.name << " silent for " << std::fixed << std::setprecision(1)
       << std::chrono::duration<double>(silent).count() << " s";
  const std::string message = text.str();
  logger.error(message);
  recorder.request("hang-" + entry.name);
  // Last: a hung bus handler may block the publish.
  Alarm alarm;
  alarm.source = "hang";
  alarm.severity = AlarmSeverity::Warning;
  alarm.message = message;
  if (clock != nullptr) alarm.ts = clock->now();  // unset otherwise: the monitor has no clock of its own
  bus.publish(alarm);
}

}  // namespace nobion
