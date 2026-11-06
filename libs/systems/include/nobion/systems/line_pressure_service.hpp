#pragma once

// LinePressureService: the script host's IPressureService over an
// ExtractionLine (plan 2026-10-05, task B0), so `get_pressure` and
// `get_manometer_pressure` in a script read the line's gauges.
//
// get_pressure(controller, gauge) answers the latest reading the line has
// for `gauge` (legacy answered its cached value too; nothing is sent).
// `controller` is the gauge's driver name, or empty for any; naming another
// controller is a Config error, as is an unknown gauge. A gauge with no
// reading yet is NotConnected. A reading older than `stale_after` scan
// intervals ([system].scan_interval_ms) is an Io error naming its age:
// a gauge that stopped answering never passes off its last number as now.

#include <chrono>
#include <cstdint>
#include <string_view>

#include "nobion/devices/extraction/services.hpp"
#include "nobion/systems/extraction_line.hpp"

namespace pychron::systems {

// A reading older than this many scans is stale.
inline constexpr int kGaugeStaleScans = 3;

// How old a gauge's latest reading may be before it is stale: `scans`
// (at least 1) times the line's scan interval (at least 1 ms). The one rule
// for scripts (LinePressureService) and the canvas's stale mark.
[[nodiscard]] std::chrono::milliseconds gauge_stale_after(const config::SystemSection& system,
                                                          int scans = kGaugeStaleScans);

class LinePressureService final : public extraction::IPressureService {
 public:
  // `line` must outlive the service.
  explicit LinePressureService(const ExtractionLine& line, int stale_after = kGaugeStaleScans);

  Result<double> get_pressure(std::string_view controller, std::string_view gauge) override;
  // A manometer is a gauge here: the same as get_pressure("", name).
  Result<double> get_manometer_pressure(std::string_view name) override;

 private:
  const ExtractionLine& line_;
  int stale_after_;
};

}  // namespace pychron::systems
