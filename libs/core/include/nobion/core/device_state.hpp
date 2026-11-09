#pragma once

// The state of one device's health, as nobion::Device (libs/devices) works it
// out from its operations. Here, not in libs/devices, because the bus event
// that carries it (DeviceHealthChanged, events.hpp) is core's.

#include <string_view>

namespace nobion {

// Unknown: never operated. Ok: last operation succeeded. Degraded: recent
// operations failed, below the fault threshold. Faulted: failures reached it.
enum class DeviceState { Unknown, Ok, Degraded, Faulted };

std::string_view to_string(DeviceState state) noexcept;

}  // namespace nobion
