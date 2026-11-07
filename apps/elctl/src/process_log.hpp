#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "nobion/core/clock.hpp"
#include "nobion/core/log_dir.hpp"
#include "nobion/core/log_hub.hpp"

namespace elctl {

// Reads --install / --config / --sim from the global options before the command
// (as run() does), resolves the install (resolve_install; none is fine), loads the
// logging config, sets file_name = "elctl.log", resolves the folder, creates the hub,
// logs the notes (logger `logging`) and begin_session. A logging config that does
// not load is said on `err` and as the first error record, and the built-in
// defaults are used; no folder (HOME / LOCALAPPDATA unset) is said on `err` and
// as a warn record. Null hub, with one line on `err`, only when the back end
// itself cannot start.
// A usage error in the options is not reported here: run() reports it.
std::shared_ptr<nobion::LogHub> open_process_log(const std::vector<std::string>& args,
                                                  const nobion::Clock& clock, nobion::HostOs os,
                                                  const nobion::EnvLookup& env, std::ostream& err);

}  // namespace elctl
