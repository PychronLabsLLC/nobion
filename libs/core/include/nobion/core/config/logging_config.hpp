#pragma once

// Typed model of the optional `[logging]` table of `extraction_line.toml`.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nobion/core/config/located.hpp"
#include "nobion/core/events.hpp"

namespace nobion::config {

// `[recorder]` of logging.toml: the flight recorder (spec 5.1).
inline constexpr std::int64_t kRecorderMinMb = 4;
inline constexpr std::int64_t kRecorderMaxMb = 512;

struct RecorderConfig : Located {
  bool enabled = true;
  std::size_t size_mb = 32;  // ring bytes, kRecorderMinMb..kRecorderMaxMb
  LogLevel default_level = LogLevel::Debug;
  std::vector<std::pair<std::string, LogLevel>> levels{{"*.wire", LogLevel::Trace}};  // wire bytes always kept
};

struct LoggingConfig : Located {
  std::optional<std::filesystem::path> dir;  // nullopt = the app's default folder; empty = no file
  std::string file_name = "nobion.log";     // set by the app, not a TOML key
  std::int64_t max_size_mb = 50;
  std::int64_t max_files = 10;
  LogLevel default_level = LogLevel::Debug;
  LogLevel dock_level = LogLevel::Info;      // what the log dock shows
  std::vector<std::pair<std::string, LogLevel>> levels;  // logger-name glob -> level
  bool echo_stderr = false;
  RecorderConfig recorder;
};

}  // namespace nobion::config
