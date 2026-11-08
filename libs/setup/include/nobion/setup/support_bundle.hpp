#pragma once

// The support bundle (field diagnostics spec section 8): one zip a lab sends
// back when something goes wrong. It holds the logs, crash files and flight
// recorder dumps, the install's config (with the files edited since the
// install named), its runtime state, wire traces, the doctor report and,
// from a running nobion, its live state. Every text in it is redacted
// (nobion::redact); <root>/.nobion/credentials.toml is never read. Nothing is
// uploaded: sending the file is the operator's.
//
// Zip layout:
//   README.txt          what this is, build, host, OS, when, what was left out and why,
//                       how to take the stacks of a hung process
//   build.json          build_info(), OS, CPU, Qt version (when given), site.toml path
//   doctor.txt          doctor() without probe, one check a line; `no install` without one
//   live.json           BundleOptions::live, when set
//   logs/               *.log of the log folder (rotated ones newer than logs_since),
//                       logs/crash/, logs/flight/ (a fresh `bundle` dump when a hub is given)
//   config/             every file of the install root but runtime state, data, logs,
//                       traces and hidden folders; config/install.toml, config/site.toml,
//                       config/EDITED.txt
//   state/              *.state.toml, executor_state.json, field tables' `current` and the
//                       version it names, state/outbox.txt and state/spool.txt (names,
//                       sizes, ages; no bodies)
//   traces/             <trace dir>/*.trace, when include_traces

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "nobion/core/error.hpp"
#include "nobion/setup/doctor.hpp"
#include "nobion/setup/site.hpp"

namespace nobion {
class LogHub;
}

namespace nobion::setup {

struct BundleOptions {
  // The zip to write; empty: <log dir>/bundles/nobion-<host>-<UTC stamp>.zip.
  // Written to a temporary file beside it and renamed, so a half-written zip
  // never has this name.
  std::filesystem::path out;
  std::chrono::hours logs_since{72};       // rotated logs older than this left out
  // Over it, the oldest logs, crash files, dumps and traces are dropped (never
  // the newest log, config, state or reports) and README.txt names them.
  std::uint64_t max_bytes = 200ull << 20;
  bool include_traces = true;
  std::function<std::string()> live;       // running app's extra state (JSON); null in elctl
  // The process's hub: a fresh dump_recorder("bundle") goes in. Null: none.
  std::shared_ptr<LogHub> hub;
  // The log folder; empty: the folder of the hub's fresh dump, else
  // default_log_dir(install root, host_os(), process_env()).
  std::filesystem::path log_dir;
  // Where *.trace files are; empty: <install root>/traces (none without an install).
  std::filesystem::path trace_dir;
  // The site config to include; empty: default_site_path(). A missing file is left out.
  std::filesystem::path site_path;
  // For doctor.txt; `probe` is always off here: the bundle never touches hardware.
  DoctorOptions doctor;
  std::string qt_version;                  // nobion: qVersion(); empty: left out of build.json
};

// Writes the bundle and returns its path. Io when the zip cannot be written;
// a file that cannot be read is named in README.txt and left out.
Result<std::filesystem::path> write_support_bundle(const SiteInstall* install, const BundleOptions& options);

}  // namespace nobion::setup
