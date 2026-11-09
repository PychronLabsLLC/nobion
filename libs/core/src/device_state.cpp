#include "nobion/core/device_state.hpp"

namespace nobion {

std::string_view to_string(DeviceState state) noexcept {
  switch (state) {
    case DeviceState::Unknown: return "unknown";
    case DeviceState::Ok: return "ok";
    case DeviceState::Degraded: return "degraded";
    case DeviceState::Faulted: return "faulted";
  }
  return "unknown";
}

}  // namespace nobion
