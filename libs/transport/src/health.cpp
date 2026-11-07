#include "nobion/transport/health.hpp"

namespace nobion {

std::string_view to_string(HealthState state) noexcept {
  switch (state) {
    case HealthState::Connected: return "connected";
    case HealthState::Degraded: return "degraded";
    case HealthState::Down: return "down";
  }
  return "unknown";
}

}  // namespace nobion
