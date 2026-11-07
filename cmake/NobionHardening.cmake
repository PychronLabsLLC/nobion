# The standard library checks its own preconditions in every build
# (docs/superpowers/specs/2026-11-06-field-diagnostics-design.md section 4):
# an empty optional dereferenced, a vector or a span indexed past its end,
# front() of an empty container end in a trap, and so a crash file, instead of
# undefined behaviour.
#
#   libstdc++            _GLIBCXX_ASSERTIONS
#   libc++ with hardening modes  _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST
#                        (a capability check, not a version: upstream 17 has
#                        another macro; Apple clang on the developer's Mac
#                        has this one)
#   other libc++         nothing
#   MSVC                 nothing (its debug checks are not a release build's)
#
# NOBION_HARDENED_STDLIB is the answer, and what has_feature("hardened_stdlib")
# reports (cmake/NobionBuildInfo.cmake). A sanitizer build has the checks on
# already (cmake/NobionSanitizers.cmake, which sets the variable and wins),
# so nothing is added a second time here.
#
# NOBION_HARDENING_DEFINITIONS is the list nobion_set_warnings() puts on each
# first-party target, PRIVATE; third-party targets are left as they are.

set(NOBION_HARDENING_DEFINITIONS "")

if(NOT NOBION_SANITIZE AND NOT MSVC)
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state()
  # The headers the build compiles against: NobionToolchainFixes.cmake can
  # have swapped a stale toolchain's libc++ headers for the SDK's.
  if(NOBION_LIBCXX_CHECK_FLAGS)
    string(APPEND CMAKE_REQUIRED_FLAGS " ${NOBION_LIBCXX_CHECK_FLAGS}")
  endif()
  check_cxx_source_compiles("
#include <version>
#ifndef __GLIBCXX__
#error not libstdc++
#endif
int main() { return 0; }" NOBION_STDLIB_IS_LIBSTDCXX)
  check_cxx_source_compiles("
#include <version>
#ifndef _LIBCPP_HARDENING_MODE_FAST
#error no libc++ hardening modes
#endif
int main() { return 0; }" NOBION_STDLIB_HAS_LIBCXX_HARDENING_MODES)
  cmake_pop_check_state()

  if(NOBION_STDLIB_IS_LIBSTDCXX)
    set(NOBION_HARDENING_DEFINITIONS _GLIBCXX_ASSERTIONS)
  elseif(NOBION_STDLIB_HAS_LIBCXX_HARDENING_MODES)
    set(NOBION_HARDENING_DEFINITIONS _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
  endif()
  if(NOBION_HARDENING_DEFINITIONS)
    set(NOBION_HARDENED_STDLIB ON)
  endif()
endif()

if(NOBION_HARDENED_STDLIB)
  message(STATUS "nobion: standard library checks on")
else()
  message(STATUS "nobion: standard library checks off (not supported by this library)")
endif()
