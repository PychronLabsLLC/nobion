#include "nobion/experiment/flight_recorder_wiring.hpp"

#include <string>

#include "nobion/experiment/executor/executor.hpp"
#include "nobion/experiment/lab/session.hpp"
#include "nobion/experiment/run/state.hpp"

namespace nobion::experiment {

std::vector<SignalBus::Subscription> wire_flight_recorder(SignalBus& bus, IDumpRequester& requester) {
  std::vector<SignalBus::Subscription> subs;
  subs.push_back(bus.subscribe<executor::RunFinished>([&requester](const executor::RunFinished& e) {
    if (e.summary.state == run::RunState::Failed) requester.request("run-failed-" + e.summary.run_id);
  }));
  subs.push_back(bus.subscribe<lab::QueueEnded>([&requester](const lab::QueueEnded& e) {
    if (e.result.code == executor::kUnsafe) requester.request(executor::kUnsafe);
  }));
  return subs;
}

}  // namespace nobion::experiment
