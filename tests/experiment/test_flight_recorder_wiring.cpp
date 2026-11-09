#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nobion/core/flight_recorder_triggers.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/experiment/executor/executor.hpp"
#include "nobion/experiment/flight_recorder_wiring.hpp"
#include "nobion/experiment/lab/session.hpp"
#include "nobion/experiment/run/state.hpp"

// Spec 2026-11-06-field-diagnostics, 5.3: the experiment's events that write
// out the flight recorder.

namespace nobion::experiment {
namespace {

struct FakeRequester final : IDumpRequester {
  void request(std::string reason) override { reasons.push_back(std::move(reason)); }
  std::vector<std::string> reasons;
};

executor::RunFinished finished(std::string run_id, run::RunState state) {
  executor::RunFinished e;
  e.summary.run_id = std::move(run_id);
  e.summary.state = state;
  return e;
}

lab::QueueEnded ended(executor::QueueEnd end, std::string code) {
  lab::QueueEnded e;
  e.result.end = end;
  e.result.code = std::move(code);
  return e;
}

TEST(FlightRecorderWiring, AFailedRunDumps) {
  SignalBus bus;
  FakeRequester requester;
  const auto subs = wire_flight_recorder(bus, requester);
  bus.publish(finished("a1", run::RunState::Success));
  bus.publish(finished("a2", run::RunState::Cancelled));
  bus.publish(finished("a3", run::RunState::Failed));
  EXPECT_EQ(requester.reasons, (std::vector<std::string>{"run-failed-a3"}));
}

TEST(FlightRecorderWiring, AnUnsafeQueueDumps) {
  SignalBus bus;
  FakeRequester requester;
  const auto subs = wire_flight_recorder(bus, requester);
  bus.publish(ended(executor::QueueEnd::Completed, ""));
  bus.publish(ended(executor::QueueEnd::Failed, ""));
  bus.publish(ended(executor::QueueEnd::Aborted, executor::kUnsafe));
  EXPECT_EQ(requester.reasons, (std::vector<std::string>{"unsafe"}));
}

TEST(FlightRecorderWiring, NothingIsAskedOnceTheSubscriptionsAreGone) {
  SignalBus bus;
  FakeRequester requester;
  {
    const auto subs = wire_flight_recorder(bus, requester);
  }
  bus.publish(finished("a3", run::RunState::Failed));
  bus.publish(ended(executor::QueueEnd::Aborted, executor::kUnsafe));
  EXPECT_TRUE(requester.reasons.empty());
}

}  // namespace
}  // namespace nobion::experiment
