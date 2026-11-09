// elctl export without the DVC store (NOBION_PERSISTENCE=OFF, or Qt not
// found): the command exists and says why it does nothing.

#include <ostream>

#include "export.hpp"

namespace elctl {

int export_command(const std::vector<std::string>&, const nobion::processing::Domain&, Io io) {
  io.err << "elctl was built without persistence\n";
  return kUsage;
}

}  // namespace elctl
