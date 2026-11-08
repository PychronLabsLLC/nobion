#pragma once

// elctl support-bundle (field diagnostics spec section 8): writes the zip a
// lab sends back (setup::write_support_bundle) and prints its path.
//
//   elctl support-bundle [--install NAME] [--out FILE] [--since 72h] [--no-traces]
//
// The install is --install NAME, else the default one; with nothing installed
// the bundle is written without one (logs, site config, build). The log
// folder and the fresh flight recorder dump are the process log hub's when
// elctl has one. Never touches hardware, never uploads.
//
// Exit status: 0 written; 1 the install cannot be found or the zip cannot be
// written; 2 a usage error.

#include <optional>
#include <string>
#include <vector>

#include "cli.hpp"

namespace elctl {

int support_bundle_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io);

}  // namespace elctl
