#include "pychron/systems/line_pressure_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace pychron::systems {

std::chrono::milliseconds gauge_stale_after(const config::SystemSection& system, int scans) {
  return std::max(1, scans) * std::chrono::milliseconds(std::max<std::int64_t>(1, system.scan_interval_ms));
}

LinePressureService::LinePressureService(const ExtractionLine& line, int stale_after)
    : line_(line), stale_after_(std::max(1, stale_after)) {}

Result<double> LinePressureService::get_pressure(std::string_view controller, std::string_view gauge) {
  const auto& gauges = line_.config().gauges;
  auto g = std::find_if(gauges.begin(), gauges.end(), [&](const auto& x) { return x.name == gauge; });
  if (g == gauges.end()) return fail(ErrorKind::Config, "unknown gauge '" + std::string(gauge) + "'");
  if (!controller.empty() && controller != g->driver) {
    return fail(ErrorKind::Config, "gauge '" + g->name + "' is on '" + g->driver + "', not '" +
                                       std::string(controller) + "'", g->name);
  }
  auto latest = line_.latest_pressure(g->name);
  if (!latest) return fail(ErrorKind::NotConnected, "gauge '" + g->name + "' has no reading yet", g->name);
  const auto age = line_.clock().now() - latest->ts;
  if (age > gauge_stale_after(line_.config().system, stale_after_)) {
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.1f", std::chrono::duration<double>(age).count());
    return fail(ErrorKind::Io, "gauge '" + g->name + "' last read " + seconds + " s ago; it is not answering",
                g->name);
  }
  return latest->value;
}

Result<double> LinePressureService::get_manometer_pressure(std::string_view name) { return get_pressure({}, name); }

}  // namespace pychron::systems
