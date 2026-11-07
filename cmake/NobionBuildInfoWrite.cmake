# Run with cmake -P by the custom command of nobion_generate_build_info.
# Inputs: SOURCE_DIR GIT_EXECUTABLE VERSION TEMPLATE FEATURES_FILE OUT.

set(sha "")
set(dirty "false")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE rc ERROR_QUIET)
  if(rc EQUAL 0 AND head MATCHES "^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]*$")
    string(SUBSTRING "${head}" 0 12 sha)
    string(TOLOWER "${sha}" sha)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0 OR NOT "${changes}" STREQUAL "")
      set(dirty "true")
    endif()
  endif()
endif()
# Only 12 or more hex digits and nothing else: the value ends up inside a C++
# string literal, and the header promises 12 hex digits or "unknown".
if(sha STREQUAL "" AND "$ENV{GITHUB_SHA}" MATCHES "^[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]*$")
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 12 sha)
  string(TOLOWER "${sha}" sha)
endif()
if(sha STREQUAL "")
  set(sha "unknown")
endif()

set(FEATURE_COUNT 0)
set(FEATURE_LIST "")
file(STRINGS "${FEATURES_FILE}" names)
foreach(name IN LISTS names)
  if(NOT name STREQUAL "")
    string(APPEND FEATURE_LIST "    \"${name}\",\n")
    math(EXPR FEATURE_COUNT "${FEATURE_COUNT}+1")
  endif()
endforeach()

set(GIT_SHA "${sha}")
set(GIT_DIRTY "${dirty}")
configure_file("${TEMPLATE}" "${OUT}" @ONLY)
