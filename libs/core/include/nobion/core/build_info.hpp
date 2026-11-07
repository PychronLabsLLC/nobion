#pragma once

#include <span>
#include <string>
#include <string_view>

namespace nobion {

// Which source built this program. Generated at build time
// (cmake/NobionBuildInfo.cmake); every string lives for the whole process.
struct BuildInfo {
  std::string_view version;     // PROJECT_VERSION
  std::string_view git_sha;     // 12 hex digits, or "unknown"
  std::string_view build_type;  // CMAKE_BUILD_TYPE, or the config of a multi-config generator
  bool git_dirty;               // tracked files differed from HEAD when built
  std::span<const std::string_view> features;  // optional parts compiled in
};

[[nodiscard]] const BuildInfo& build_info() noexcept;

// "0.5.0+3f2a9c1e04b7" or "0.5.0+3f2a9c1e04b7.dirty".
[[nodiscard]] std::string build_id();

[[nodiscard]] bool has_feature(std::string_view name);

}  // namespace nobion
