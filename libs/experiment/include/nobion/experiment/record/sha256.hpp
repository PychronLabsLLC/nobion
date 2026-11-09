#pragma once

#include <string>
#include <string_view>

namespace nobion::experiment::record {

// Lower-case hex SHA-256 of `data`.
std::string sha256_hex(std::string_view data);

}  // namespace nobion::experiment::record
