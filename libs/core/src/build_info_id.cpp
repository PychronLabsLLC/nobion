#include <algorithm>
#include <string>
#include <string_view>

#include "nobion/core/build_info.hpp"

namespace nobion {

std::string build_id() {
  const BuildInfo& info = build_info();
  std::string id;
  id.append(info.version).append("+").append(info.git_sha);
  if (info.git_dirty) {
    id += ".dirty";
  }
  return id;
}

bool has_feature(std::string_view name) {
  const auto features = build_info().features;
  return std::ranges::find(features, name) != features.end();
}

}  // namespace nobion
