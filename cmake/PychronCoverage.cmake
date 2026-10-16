# PYCHRON_COVERAGE builds every target, third-party ones included, with
# clang's source-based coverage (-fprofile-instr-generate -fcoverage-mapping),
# as PychronSanitizers.cmake does for the sanitizers. Used by the `coverage`
# presets and read by tools/coverage_report.py; not in CI.
#
# Clang and AppleClang only: gcc's gcov is another format the report does not
# read. Not combined with PYCHRON_SANITIZE: instrumented counters under ASan
# cost both twice and measure nothing the plain build does not.
#
# -fcoverage-compilation-dir pins the build directory into each object's
# coverage mapping and so into ccache's key: with ccache's base_dir the
# compiler sees source paths relative to the build directory, and an object
# reused from another worktree's coverage build would otherwise name that
# worktree's directory.
option(PYCHRON_COVERAGE "clang source-based coverage" OFF)

if(PYCHRON_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
    message(FATAL_ERROR
      "PYCHRON_COVERAGE needs Clang or AppleClang; the C++ compiler is "
      "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER})")
  endif()
  if(PYCHRON_SANITIZE)
    message(FATAL_ERROR
      "PYCHRON_COVERAGE is not combined with PYCHRON_SANITIZE "
      "(${PYCHRON_SANITIZE}): configure one build directory for each")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping
    "-fcoverage-compilation-dir=${CMAKE_BINARY_DIR}")
  add_link_options(-fprofile-instr-generate)
  message(STATUS "pychron: clang source-based coverage enabled")
endif()
