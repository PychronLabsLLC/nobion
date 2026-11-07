#pragma once

// Assembles one extraction line from its config: transports -> drivers ->
// SwitchManager + GaugeScanner, sharing one Clock, SignalBus and Scheduler.
//
// This is elctl's stand-in for the ExtractionLine facade (spec 9.2, work
// step 10), which has not landed yet; once it does, elctl should build that
// instead and this file goes away.
//
// `kind = "sim"` transports (or every transport, with force_sim) are wired to
// minimal stateless-per-process device models chosen by the kind of driver
// on the transport: a ProxrBoardSim for `proxr_relay`, a fixed-pressure
// MaxiGauge for `pfeiffer_maxigauge`, a Plc2000HeaterSim for
// `plc2000_heater`. SimSystem replaces these later.

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/config/system_config.hpp"
#include "nobion/core/error.hpp"
#include "nobion/core/event_recorder.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/scheduler.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/devices/device.hpp"
#include "nobion/devices/heater.hpp"
#include "nobion/experiment/event_log.hpp"
#include "nobion/systems/event_log.hpp"
#include "nobion/systems/gauge_scanner.hpp"
#include "nobion/systems/switch_manager.hpp"
#include "nobion/transport/endpoints.hpp"
#include "nobion/transport/transport.hpp"
#include "trace_settings.hpp"

namespace elctl {

struct LineOptions {
  bool force_sim = false;  // treat every transport as kind = "sim"
  // A line served by `elctl sim serve`: transports connect where it answers
  // (nobion::make_transport, TransportContext::endpoints). Null: as configured.
  std::shared_ptr<const std::map<std::string, nobion::Endpoint>> sim_endpoints;
  TraceSettings trace;     // transports to record, on top of config `trace = true`
  std::filesystem::path trace_dir = "traces";
};

class Line {
 public:
  static nobion::Result<std::unique_ptr<Line>> build(nobion::config::SystemConfig config, LineOptions options = {});

  ~Line();
  Line(const Line&) = delete;
  Line& operator=(const Line&) = delete;

  struct Opened {
    std::string transport;
    nobion::Result<void> result;
  };
  // Opens every transport, in config order, and reports each outcome.
  std::vector<Opened> open_all();
  void close_all();

  const nobion::config::SystemConfig& config() const noexcept { return config_; }
  nobion::SignalBus& bus() noexcept { return bus_; }
  const nobion::Clock& clock() const noexcept { return clock_; }

  nobion::Transport* transport(const std::string& name) const;
  nobion::Device* device(const std::string& name) const;
  nobion::systems::SwitchManager& switches() noexcept { return *switches_; }

  const nobion::config::GaugeConfig* gauge(const std::string& name) const;
  // One reading, in the gauge's configured units.
  nobion::Result<double> read_gauge(const std::string& name);

  const nobion::config::HeaterConfig* heater_config(const std::string& name) const;
  // Config error for an unknown heater or a driver that is not one.
  nobion::Result<nobion::IHeater*> heater(const std::string& name) const;

  // Registers every gauge with the scanner at `interval` and starts the
  // scheduler; PressureSample/Alarm arrive on bus(). stop_scan() undoes it.
  nobion::Result<void> start_scan(nobion::Duration interval);
  void stop_scan();

 private:
  struct Sims;
  Line(nobion::config::SystemConfig config);

  nobion::config::SystemConfig config_;
  // Declaration order is teardown order in reverse: the scanner and
  // scheduler stop before the drivers they call, drivers go before their
  // transports, and transports before the sim models their hooks borrow.
  nobion::SteadyClock clock_;
  nobion::SignalBus bus_;
  // After the bus and before the transports: destroyed after the transports
  // that log through it. No [logging].dir means no file sink.
  std::shared_ptr<nobion::LogHub> log_hub_;
  // The process hub's bus attachment: declared after bus_, so it goes first.
  std::optional<nobion::LogHub::BusAttachment> bus_attachment_;
  // The bus's events to the process log (spec 2.1): made once the process hub
  // is known, declared after bus_ so they go before it.
  std::optional<nobion::EventRecorder> core_events_;
  std::optional<nobion::systems::SystemsEventRecorder> systems_events_;
  std::optional<nobion::experiment::ExperimentEventRecorder> experiment_events_;
  std::unique_ptr<Sims> sims_;
  std::vector<std::pair<std::string, std::unique_ptr<nobion::Transport>>> transports_;
  std::vector<std::pair<std::string, std::unique_ptr<nobion::Device>>> devices_;
  std::unique_ptr<nobion::systems::SwitchManager> switches_;
  std::unique_ptr<nobion::Scheduler> scheduler_;
  std::unique_ptr<nobion::GaugeScanner> scanner_;
};

}  // namespace elctl
