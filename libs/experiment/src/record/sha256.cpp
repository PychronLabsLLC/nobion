#include "nobion/experiment/record/sha256.hpp"

#include "nobion/core/sha256.hpp"

namespace nobion::experiment::record {

std::string sha256_hex(std::string_view data) { return to_hex(sha256(data)); }

}  // namespace nobion::experiment::record
