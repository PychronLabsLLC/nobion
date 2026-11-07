#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/hang_monitor.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/core/virtual_clock.hpp"
#include "nobion/transport/sim_transport.hpp"
#include "nobion/transport/transport.hpp"
#include "virtual_time.hpp"

using namespace nobion;
using namespace std::chrono_literals;

namespace {

// Primitive-level fake: scripted results for each do_* call.
class FakeChannel final : public QueuedTransport {
 public:
  explicit FakeChannel(TransportOptions o) : QueuedTransport(std::move(o)) {}
  ~FakeChannel() override { shutdown(); }
  using QueuedTransport::shutdown;

  Result<void> open_result;
  std::deque<Result<Bytes>> reads;
  std::deque<Result<void>> writes;
  std::function<void()> on_read;

  std::atomic<int> write_calls{0}, read_calls{0}, discard_calls{0}, close_calls{0};
  std::atomic<bool> in_flight{false}, overlap{false};
  Duration last_timeout{};

 private:
  Result<void> do_open() override { return open_result; }
  void do_close() override { ++close_calls; }
  void do_discard_input() override { ++discard_calls; }
  Result<void> do_write(const Bytes&, Duration) override {
    if (in_flight.exchange(true)) overlap = true;
    ++write_calls;
    if (writes.empty()) return {};
    auto r = writes.front();
    writes.pop_front();
    return r;
  }
  Result<Bytes> do_read(const ReadSpec&, Duration timeout) override {
    ++read_calls;
    last_timeout = timeout;
    if (on_read) on_read();
    in_flight = false;
    if (reads.empty()) return fail(ErrorKind::Timeout, "no reply");
    auto r = reads.front();
    reads.pop_front();
    return r;
  }
};

TransportOptions opts(int retries = 0, std::uint64_t down_after = 3) {
  TransportOptions o;
  o.name = "bus1";
  o.timeout = 250ms;
  o.retries = retries;
  o.down_after = down_after;
  return o;
}

const ReadSpec kCr = ReadSpec::until("\r");

using nobion::testing::await_waiters;

// A transport that waits past the clock leaves time standing and the test
// asleep in it. Each test on a VirtualClock runs under a real-time bound: when
// it is exceeded the process says so and aborts, well inside the ctest timeout.
class QueuedTransportVirtual : public nobion::testing::VirtualTimeTest {};

}  // namespace

TEST(QueuedTransport, ExchangeBeforeOpenIsNotConnected) {
  FakeChannel t(opts());
  auto r = t.exchange(to_bytes("x"), kCr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::NotConnected);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_FALSE(t.write(to_bytes("x")));
  EXPECT_FALSE(t.read(kCr));
}

TEST(QueuedTransport, ExchangeDiscardsWritesThenReads) {
  ManualClock clock(TimePoint{} + 10s);
  auto o = opts();
  o.clock = &clock;
  FakeChannel t(o);
  ASSERT_TRUE(t.open());
  t.reads.emplace_back(to_bytes("OK\r"));
  auto r = t.exchange(to_bytes("PR1\r"), kCr);
  ASSERT_TRUE(r) << to_string(r.error());
  EXPECT_EQ(to_string(*r), "OK\r");
  EXPECT_EQ(t.discard_calls, 1);
  EXPECT_EQ(t.write_calls, 1);
  const auto h = t.health();
  EXPECT_EQ(h.state, HealthState::Connected);
  EXPECT_EQ(h.last_ok, TimePoint{} + 10s);
  EXPECT_EQ(h.consecutive_failures, 0u);
}

TEST(QueuedTransport, ZeroTimeoutUsesConfiguredDefault) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  t.reads.emplace_back(Bytes{});
  ASSERT_TRUE(t.exchange(to_bytes("a"), kCr));
  EXPECT_EQ(t.last_timeout, Duration(250ms));
  t.reads.emplace_back(Bytes{});
  ASSERT_TRUE(t.exchange(to_bytes("a"), kCr, 40ms));
  EXPECT_EQ(t.last_timeout, Duration(40ms));
}

TEST(QueuedTransport, ResendsAfterATimeoutWhenAskedThenSucceeds) {
  FakeChannel t(opts(/*retries=*/2));
  ASSERT_TRUE(t.open());
  t.reads.emplace_back(fail(ErrorKind::Timeout, "t1"));
  t.reads.emplace_back(fail(ErrorKind::Timeout, "t2"));
  t.reads.emplace_back(to_bytes("OK\r"));
  auto r = t.exchange(to_bytes("q"), kCr, Transport::kDefaultTimeout, Resend::Allowed);
  ASSERT_TRUE(r);
  EXPECT_EQ(t.write_calls, 3);
  EXPECT_EQ(t.discard_calls, 3);
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(QueuedTransport, RetriesExhaustedReportsLastErrorOnce) {
  FakeChannel t(opts(/*retries=*/1));
  ASSERT_TRUE(t.open());
  auto r = t.exchange(to_bytes("q"), kCr, Transport::kDefaultTimeout, Resend::Allowed);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.write_calls, 2);
  const auto h = t.health();
  EXPECT_EQ(h.state, HealthState::Degraded);
  EXPECT_EQ(h.consecutive_failures, 1u);
  EXPECT_NE(h.last_error.find("no reply"), std::string::npos);
}

TEST(QueuedTransport, ProtocolErrorsAreNotRetried) {
  FakeChannel t(opts(/*retries=*/3));
  ASSERT_TRUE(t.open());
  t.writes.emplace_back(fail(ErrorKind::Protocol, "rejected"));
  auto r = t.exchange(to_bytes("q"), kCr, Transport::kDefaultTimeout, Resend::Allowed);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(t.write_calls, 1);
  EXPECT_EQ(t.read_calls, 0);
}

// A write has no reply to tell "not sent" from "sent and lost": the caller,
// who knows whether the command may arrive twice, is the one to send it again.
TEST(QueuedTransport, WriteIsNeverSentTwice) {
  FakeChannel t(opts(/*retries=*/3));
  ASSERT_TRUE(t.open());
  t.writes.emplace_back(fail(ErrorKind::Timeout, "stalled"));
  auto r = t.write(to_bytes("w"));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
  EXPECT_EQ(t.write_calls, 1);
}

TEST(QueuedTransport, ExchangeRepeatsOnlyWhenAsked) {
  {
    FakeChannel t(opts(/*retries=*/3));
    ASSERT_TRUE(t.open());
    EXPECT_FALSE(t.exchange(to_bytes("q"), kCr));
    EXPECT_EQ(t.write_calls, 1);
  }
  {
    FakeChannel t(opts(/*retries=*/3));
    ASSERT_TRUE(t.open());
    auto r = t.exchange(to_bytes("q"), kCr, Transport::kDefaultTimeout, Resend::Allowed);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(t.write_calls, 4);
  }
  {
    // The channel is closed by the Io: there is nothing left to resend on.
    FakeChannel t(opts(/*retries=*/3));
    ASSERT_TRUE(t.open());
    t.reads.emplace_back(fail(ErrorKind::Io, "reset by peer"));
    auto r = t.exchange(to_bytes("q"), kCr, Transport::kDefaultTimeout, Resend::Allowed);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Io);
    EXPECT_EQ(t.write_calls, 1);
  }
}

TEST(QueuedTransport, AnIoErrorClosesTheChannel) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  t.reads.emplace_back(fail(ErrorKind::Io, "reset by peer"));
  auto r = t.exchange(to_bytes("q"), kCr);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(t.close_calls, 1);
  EXPECT_EQ(t.health().state, HealthState::Down);

  auto next = t.exchange(to_bytes("q"), kCr);
  ASSERT_FALSE(next);
  EXPECT_EQ(next.error().kind, ErrorKind::NotConnected);
  EXPECT_EQ(t.write_calls, 1);
  EXPECT_EQ(t.close_calls, 1);

  // It can be opened again, which is how a link comes back.
  ASSERT_TRUE(t.open());
  t.reads.emplace_back(to_bytes("OK\r"));
  EXPECT_TRUE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Connected);
}

TEST(QueuedTransport, AnIoErrorOnAWriteOrAReadClosesTheChannel) {
  {
    FakeChannel t(opts());
    ASSERT_TRUE(t.open());
    t.writes.emplace_back(fail(ErrorKind::Io, "broken pipe"));
    EXPECT_FALSE(t.write(to_bytes("w")));
    EXPECT_EQ(t.close_calls, 1);
    EXPECT_EQ(t.write(to_bytes("w")).error().kind, ErrorKind::NotConnected);
  }
  {
    FakeChannel t(opts());
    ASSERT_TRUE(t.open());
    t.reads.emplace_back(fail(ErrorKind::Io, "eof"));
    EXPECT_FALSE(t.read(kCr));
    EXPECT_EQ(t.close_calls, 1);
    EXPECT_EQ(t.read(kCr).error().kind, ErrorKind::NotConnected);
  }
  {
    FakeChannel t(opts());
    ASSERT_TRUE(t.open());
    t.reads.emplace_back(fail(ErrorKind::Io, "eof"));
    EXPECT_FALSE(t.poll(kCr));
    EXPECT_EQ(t.close_calls, 1);
    EXPECT_EQ(t.health().state, HealthState::Down);
  }
}

TEST(QueuedTransport, ATimeoutLeavesTheChannelOpen) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  EXPECT_EQ(t.exchange(to_bytes("q"), kCr).error().kind, ErrorKind::Timeout);
  EXPECT_EQ(t.close_calls, 0);
  EXPECT_EQ(t.health().state, HealthState::Degraded);
  t.reads.emplace_back(to_bytes("OK\r"));
  EXPECT_TRUE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.write_calls, 2);
}

TEST(QueuedTransport, GoesDownAfterThresholdAndRecovers) {
  FakeChannel t(opts(0, /*down_after=*/2));
  ASSERT_TRUE(t.open());
  EXPECT_FALSE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Degraded);
  EXPECT_FALSE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_EQ(t.health().consecutive_failures, 2u);
  t.reads.emplace_back(to_bytes("OK\r"));
  EXPECT_TRUE(t.exchange(to_bytes("q"), kCr));
  EXPECT_EQ(t.health().state, HealthState::Connected);
  EXPECT_EQ(t.health().consecutive_failures, 0u);
}

TEST(QueuedTransport, OpenFailureLeavesTransportDown) {
  FakeChannel t(opts());
  t.open_result = fail(ErrorKind::Io, "no such port");
  auto r = t.open();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().device, "bus1");
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_NE(t.health().last_error.find("no such port"), std::string::npos);
  EXPECT_EQ(t.exchange(to_bytes("q"), kCr).error().kind, ErrorKind::NotConnected);
}

TEST(QueuedTransport, CloseMakesFurtherCallsNotConnected) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  t.close();
  EXPECT_EQ(t.close_calls, 1);
  EXPECT_EQ(t.health().state, HealthState::Down);
  EXPECT_EQ(t.exchange(to_bytes("q"), kCr).error().kind, ErrorKind::NotConnected);
  t.close();  // idempotent
  EXPECT_EQ(t.close_calls, 1);
}

TEST(QueuedTransport, PublishesHealthOnStateChange) {
  SignalBus bus;
  std::vector<TransportHealth> events;
  auto sub = bus.subscribe<TransportHealth>([&](const TransportHealth& e) { events.push_back(e); });
  auto o = opts(0, 2);
  o.bus = &bus;
  FakeChannel t(o);
  ASSERT_TRUE(t.open());
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(events[0].connected);
  EXPECT_EQ(events[0].transport, "bus1");

  (void)t.exchange(to_bytes("q"), kCr);  // Degraded
  (void)t.exchange(to_bytes("q"), kCr);  // Down
  (void)t.exchange(to_bytes("q"), kCr);  // still Down: no event
  ASSERT_EQ(events.size(), 3u);
  EXPECT_TRUE(events[1].connected);
  EXPECT_EQ(events[1].error_count, 1u);
  EXPECT_FALSE(events[2].connected);
  EXPECT_EQ(events[2].error_count, 2u);
  EXPECT_FALSE(events[2].last_error.empty());
}

TEST(QueuedTransport, ConcurrentExchangesAreSerialized) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  constexpr int kThreads = 8, kEach = 50;
  for (int i = 0; i < kThreads * kEach; ++i) t.reads.emplace_back(to_bytes("OK\r"));
  t.on_read = [] { std::this_thread::yield(); };
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < kEach; ++j)
        if (t.exchange(to_bytes("q"), kCr)) ++ok;
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(ok, kThreads * kEach);
  EXPECT_FALSE(t.overlap);
}

TEST(QueuedTransport, ShutdownCancelsQueuedAndLaterCalls) {
  FakeChannel t(opts());
  ASSERT_TRUE(t.open());
  std::promise<void> entered, release;
  auto release_f = release.get_future().share();
  std::atomic<bool> first_read{true};
  t.on_read = [&, release_f] {
    if (!first_read.exchange(false)) return;
    entered.set_value();
    release_f.wait();
  };
  t.reads.emplace_back(to_bytes("first\r"));

  auto first = std::async(std::launch::async, [&] { return t.exchange(to_bytes("1"), kCr); });
  entered.get_future().wait();  // worker is now busy with the first call
  auto second = std::async(std::launch::async, [&] { return t.exchange(to_bytes("2"), kCr); });
  std::this_thread::sleep_for(50ms);  // let the second call enqueue

  std::thread stopper([&] { t.shutdown(); });
  std::this_thread::sleep_for(20ms);
  release.set_value();
  stopper.join();

  EXPECT_TRUE(first.get());
  auto r2 = second.get();
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(t.exchange(to_bytes("3"), kCr).error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(t.close_calls, 1);
}

TEST_F(QueuedTransportVirtual, CallerWaitsForTheWorkerInClockTime) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  auto t = SimTransport::hooked(
      [&](const Bytes&) {
        clock.sleep_for(2s);  // the instrument takes its time, on the worker
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());

  const auto real_start = std::chrono::steady_clock::now();
  auto r = t->exchange(to_bytes("Q\r"), kCr);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "OK\r");
  EXPECT_EQ(clock.now(), kStart + 2s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// The call the worker is busy with is finished, as on any clock; the one
// queued behind it is the pending call, and it is woken with Cancelled at
// once, without the clock moving.
TEST_F(QueuedTransportVirtual, ShutdownCancelsAPendingCall) {
  VirtualClock clock;
  Clock::Participant main(clock, "test");  // runnable until it sleeps: time stands
  const TimePoint kStart = clock.now();
  auto o = opts();
  o.clock = &clock;
  auto t = SimTransport::hooked(
      [&](const Bytes&) {
        clock.sleep_for(2s);
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());
  SimTransport* const raw = t.get();

  auto first = std::async(std::launch::async, [&clock, raw] {
    Clock::Participant caller(clock, "first");
    return raw->exchange(to_bytes("1\r"), kCr);
  });
  ASSERT_TRUE(await_waiters(clock, 2));  // the worker in the hook, and its caller
  auto second = std::async(std::launch::async, [&clock, raw] {
    Clock::Participant caller(clock, "second");
    return raw->exchange(to_bytes("2\r"), kCr);
  });
  ASSERT_TRUE(await_waiters(clock, 3));  // and the caller queued behind it

  std::thread destroyer([&t] { t.reset(); });  // not a participant
  auto r2 = second.get();
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().kind, ErrorKind::Cancelled);
  EXPECT_EQ(clock.now(), kStart);

  clock.sleep_for(2s);  // lets the worker out of the hook
  auto r1 = first.get();
  ASSERT_TRUE(r1) << r1.error().what;
  EXPECT_EQ(to_string(*r1), "OK\r");
  destroyer.join();
  EXPECT_EQ(clock.now(), kStart + 2s);
}

// Hang monitor (spec 2026-11-06-field-diagnostics, 6): a call that does not
// return is reported as `transport.<name>`; an idle transport, or one on a
// VirtualClock, is not watched. Real time; each wait for a report is bounded
// at 5 s, there only to tell a report from none.
namespace {

std::shared_ptr<LogHub> hang_hub(const Clock& clock) {
  config::LoggingConfig cfg;
  cfg.dir = std::filesystem::path{};  // no file: nothing to clean up
  auto hub = LogHub::create(cfg, clock);
  EXPECT_TRUE(hub.has_value());
  return hub.has_value() ? *hub : nullptr;
}

// The dumps the monitor asked for.
class HangRequests final : public IDumpRequester {
 public:
  void request(std::string reason) override {
    {
      const std::lock_guard lock(mutex_);
      reasons_.push_back(std::move(reason));
    }
    cv_.notify_all();
  }
  bool await(const std::string& reason) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 5s, [&] { return count_locked(reason) > 0; });
  }
  std::size_t count(const std::string& reason) {
    const std::lock_guard lock(mutex_);
    return count_locked(reason);
  }

 private:
  std::size_t count_locked(const std::string& reason) const {
    return static_cast<std::size_t>(std::count(reasons_.begin(), reasons_.end(), reason));
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> reasons_;
};

// Holds a hook until opened.
class Gate {
 public:
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  void pass() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
  }
  // Waits, at most 5 s, until a hook is held.
  bool await_entered() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, 5s, [this] { return entered_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
  bool entered_ = false;
};

}  // namespace

TEST(QueuedTransport, AStuckCallIsReported) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = hang_hub(clock);
  ASSERT_TRUE(hub);
  HangRequests requests;
  HangMonitor monitor(*hub, requests, bus, HangMonitor::Options{50ms});
  Gate gate;
  auto o = opts();
  o.hang_monitor = &monitor;
  o.hang_limit = 200ms;
  auto t = SimTransport::hooked(
      [&gate](const Bytes&) {
        gate.pass();
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());

  auto call = std::async(std::launch::async, [raw = t.get()] { return raw->exchange(to_bytes("Q\r"), kCr); });
  const bool reported = requests.await("hang-transport.bus1");
  gate.open();
  EXPECT_TRUE(reported);
  const auto r = call.get();
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_EQ(to_string(*r), "OK\r");
  EXPECT_EQ(requests.count("hang-transport.bus1"), 1U);
}

TEST(QueuedTransport, AnIdleTransportIsNotWatched) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = hang_hub(clock);
  ASSERT_TRUE(hub);
  HangRequests requests;
  HangMonitor monitor(*hub, requests, bus, HangMonitor::Options{50ms});
  auto o = opts();
  o.hang_monitor = &monitor;
  o.hang_limit = 50ms;
  FakeChannel t(o);
  const auto control = monitor.beat("control", 50ms);

  ASSERT_TRUE(requests.await("hang-control"));
  EXPECT_EQ(requests.count("hang-transport.bus1"), 0U);
}

// Under a VirtualClock a simulated wait has no fixed real-time length: the
// transport registers no beat, however long a call takes in real time.
TEST(QueuedTransport, ATransportOnAVirtualClockIsNotWatched) {
  const SteadyClock steady;
  VirtualClock clock;
  SignalBus bus;
  auto hub = hang_hub(steady);
  ASSERT_TRUE(hub);
  HangRequests requests;
  HangMonitor monitor(*hub, requests, bus, HangMonitor::Options{50ms});
  Gate gate;
  auto o = opts();
  o.clock = &clock;
  o.hang_monitor = &monitor;
  o.hang_limit = 50ms;
  auto t = SimTransport::hooked(
      [&gate](const Bytes&) {
        gate.pass();
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());

  auto call = std::async(std::launch::async, [raw = t.get()] { return raw->exchange(to_bytes("Q\r"), kCr); });
  const bool entered = gate.await_entered();
  const auto control = monitor.beat("control", 50ms);
  const bool control_reported = requests.await("hang-control");
  gate.open();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(control_reported);
  EXPECT_EQ(requests.count("hang-transport.bus1"), 0U);
  const auto r = call.get();
  ASSERT_TRUE(r) << r.error().what;
}

// The beat measures silence between wire calls, not a job's length: a
// transaction of short exchanges that together outlast the limit is not a
// hang. Each exchange takes 150 ms against a 600 ms limit; six take 900 ms.
TEST(QueuedTransport, ATransactionOfShortExchangesIsNotReported) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = hang_hub(clock);
  ASSERT_TRUE(hub);
  HangRequests requests;
  HangMonitor monitor(*hub, requests, bus, HangMonitor::Options{50ms});
  auto o = opts();
  o.hang_monitor = &monitor;
  o.hang_limit = 600ms;
  auto t = SimTransport::hooked(
      [](const Bytes&) {
        std::this_thread::sleep_for(150ms);  // the instrument takes its time
        return to_bytes("OK\r");
      },
      o);
  ASSERT_TRUE(t->open());
  SimTransport* const raw = t.get();
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw->transaction([raw]() -> Result<void> {
    for (int i = 0; i < 6; ++i) {
      auto reply = raw->exchange(to_bytes("Q\r"), kCr);
      if (!reply) return fail(reply.error());
    }
    return {};
  });
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_GT(std::chrono::steady_clock::now() - started, 600ms);  // the job did outlast the limit

  const auto control = monitor.beat("control", 50ms);  // the monitor has looked again since
  ASSERT_TRUE(requests.await("hang-control"));
  EXPECT_EQ(requests.count("hang-transport.bus1"), 0U);
}

// A read given a timeout longer than the configured one is allowed ten times
// its own: waiting it out is not a hang.
TEST(QueuedTransport, ALongExplicitTimeoutIsNotAHang) {
  const SteadyClock clock;
  SignalBus bus;
  auto hub = hang_hub(clock);
  ASSERT_TRUE(hub);
  HangRequests requests;
  HangMonitor monitor(*hub, requests, bus, HangMonitor::Options{50ms});
  auto o = opts();
  o.hang_monitor = &monitor;
  o.hang_limit = 100ms;
  auto t = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, o, [] { return Bytes{}; });
  ASSERT_TRUE(t->open());
  const auto r = t->read(kCr, 600ms);  // nothing comes: waits the whole 600 ms
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);

  const auto control = monitor.beat("control", 50ms);
  ASSERT_TRUE(requests.await("hang-control"));
  EXPECT_EQ(requests.count("hang-transport.bus1"), 0U);
}
