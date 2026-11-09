#pragma once

#include <string_view>

#include "nobion/core/clock.hpp"
#include "nobion/core/error.hpp"

namespace elctl {

// "250ms", "3s", "1.5s", "2m", "1h"; a bare number is seconds. Config error
// for anything else, including negative values.
nobion::Result<nobion::Duration> parse_duration(std::string_view text);

}  // namespace elctl
