#include "nobion/core/hang_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include "nobion/core/events.hpp"
#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_hub.hpp"
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
  HangEntry(std::string n, std::chrono::nanoseconds l) : name(std::move(n)), limit(l) {}

  const std::string name;
  const std::chrono::nanoseconds limit;
  // The last pulse (steady_stamp), or 0 while disarmed. The one thing the
  // watched thread writes.
  std::atomic<std::int64_t> last{steady_stamp()};
  // The pulse whose silence was last reported. Monitor thread only.
  std::int64_t reported = 0;
};

struct HangState {
  std::mutex mutex;  // guards the two below; never held across a report
  std::condition_variable cv;
  bool stopping = false;
  std::vector<std::shared_ptr<HangEntry>> entries;
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
  if (entry_) entry_->last.store(steady_stamp());
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

HangMonitor::HangMonitor(LogHub& log, IDumpRequester& recorder, SignalBus& bus, Options options)
    : logger_(log.logger("hang")),
      recorder_(recorder),
      bus_(bus),
      options_(options),
      state_(std::make_shared<detail::HangState>()) {
  options_.poll = std::max(options_.poll, std::chrono::milliseconds(1));
  thread_ = std::thread([this] { run(); });
}

HangMonitor::~HangMonitor() {
  {
    const std::lock_guard lock(state_->mutex);
    state_->stopping = true;
  }
  state_->cv.notify_all();
  thread_.join();
}

HangMonitor::Beat HangMonitor::beat(std::string name, std::chrono::milliseconds limit) {
  auto entry = std::make_shared<detail::HangEntry>(std::move(name), limit);
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
      entries = state_->entries;
    }
    for (const auto& entry : entries) {
      const std::int64_t last = entry->last.load();
      if (last == 0 || last == entry->reported) continue;
      const auto silent = std::chrono::nanoseconds(steady_stamp() - last);
      if (silent <= entry->limit) continue;
      entry->reported = last;  // once per episode: the next report needs a pulse
      report(*entry, silent);
    }
    entries.clear();  // a beat let go is not kept alive here until the next poll
  }
}

void HangMonitor::report(detail::HangEntry& entry, std::chrono::steady_clock::duration silent) {
  std::ostringstream text;
  text << "hang: " << entry.name << " silent for " << std::fixed << std::setprecision(1)
       << std::chrono::duration<double>(silent).count() << " s";
  const std::string message = text.str();
  logger_.error(message);
  recorder_.request("hang-" + entry.name);
  // Last: a hung bus handler may block the publish.
  Alarm alarm;
  alarm.source = "hang";
  alarm.severity = AlarmSeverity::Warning;
  alarm.message = message;
  // ts left unset: the monitor has no Clock (it is on real time, whatever the
  // line's), and a steady_clock stamp would not match the line's under a
  // VirtualClock.
  bus_.publish(alarm);
}

}  // namespace nobion
