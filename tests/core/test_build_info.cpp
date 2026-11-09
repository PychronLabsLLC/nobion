#include "nobion/core/build_info.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <version>

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

// The feature is on exactly where the standard library's own checks are on in
// this translation unit: libstdc++ asserts, libc++ with hardening modes at a
// mode other than none (cmake/NobionHardening.cmake; a sanitizer build has them
// on as well). The mode's value, not whether it is defined: libc++'s <version>
// defines _LIBCPP_HARDENING_MODE to its default, which is none, when nothing
// asked for hardening (Apple clang does).
TEST(BuildInfo, HardenedStdlibOnWhereSupported) {
#if defined(_GLIBCXX_ASSERTIONS)
  constexpr bool checks_on = true;
#elif defined(_LIBCPP_HARDENING_MODE) && defined(_LIBCPP_HARDENING_MODE_NONE)
#if _LIBCPP_HARDENING_MODE != _LIBCPP_HARDENING_MODE_NONE
  constexpr bool checks_on = true;
#else
  constexpr bool checks_on = false;
#endif
#else
  constexpr bool checks_on = false;
#endif
  EXPECT_EQ(nobion::has_feature("hardened_stdlib"), checks_on);
#if defined(_LIBCPP_HARDENING_MODE_FAST)
  // True only if the library understood the macro the build defined.
  EXPECT_TRUE(nobion::has_feature("hardened_stdlib"));
  // FAST is what cmake/NobionHardening.cmake asks for; EXTENSIVE is what a
  // sanitizer build asks for instead (cmake/NobionSanitizers.cmake).
  EXPECT_TRUE(_LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_FAST ||
              _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_EXTENSIVE);
#elif defined(__GLIBCXX__)
  EXPECT_TRUE(nobion::has_feature("hardened_stdlib"));
#endif
}

TEST(BuildInfo, BuildTypeIsNamed) { EXPECT_FALSE(nobion::build_info().build_type.empty()); }
