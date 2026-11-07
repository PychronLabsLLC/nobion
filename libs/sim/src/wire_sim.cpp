#include "nobion/sim/wire_sim.hpp"

#include <utility>

namespace nobion::sim {

RequestFramer framer(ReadSpec spec) {
  return [spec = std::move(spec)](const Bytes& buffer) { return frame_length(spec, buffer); };
}

}  // namespace nobion::sim
