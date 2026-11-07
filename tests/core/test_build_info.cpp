#include "nobion/core/build_info.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

// tests/core/CMakeLists.txt gives the test target NOBION_TEST_PROJECT_VERSION
// and NOBION_TEST_HAS_METRICS.

TEST(BuildInfo, VersionIsTheProjectVersion) {
  EXPECT_EQ(nobion::build_info().version, NOBION_TEST_PROJECT_VERSION);
}

TEST(BuildInfo, ShaIsTwelveHexOrUnknown) {
  const auto sha = nobion::build_info().git_sha;
  EXPECT_TRUE(sha == "unknown" ||
              (sha.size() == 12 && std::ranges::all_of(sha, [](unsigned char c) { return std::isxdigit(c) != 0; })));
}

TEST(BuildInfo, IdJoinsVersionAndSha) {
  const auto& b = nobion::build_info();
  EXPECT_EQ(nobion::build_id(), std::string(b.version) + "+" + std::string(b.git_sha) + (b.git_dirty ? ".dirty" : ""));
}

TEST(BuildInfo, FeaturesNameWhatWasCompiledIn) {
  EXPECT_EQ(nobion::has_feature("metrics"), NOBION_TEST_HAS_METRICS != 0);
  EXPECT_FALSE(nobion::has_feature("no_such_feature"));
}

TEST(BuildInfo, BuildTypeIsNamed) { EXPECT_FALSE(nobion::build_info().build_type.empty()); }
