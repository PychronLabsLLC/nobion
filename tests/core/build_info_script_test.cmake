# ctest driver: runs cmake/NobionBuildInfoWrite.cmake with no git and a given
# GITHUB_SHA, and checks the sha it writes. Inputs: ROOT, WORK.
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/features.txt" "metrics\n")

function(sha_for value expected)
  sha_in("${WORK}" "" "${value}" "${expected}")
endfunction()

function(sha_in source git value expected)
  file(REMOVE "${WORK}/out.cpp")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GITHUB_SHA=${value}" "${CMAKE_COMMAND}"
      -DSOURCE_DIR=${source} -DGIT_EXECUTABLE=${git} -DVERSION=1.2.3
      -DTEMPLATE=${ROOT}/cmake/build_info.cpp.in -DFEATURES_FILE=${WORK}/features.txt
      -DOUT=${WORK}/out.cpp -P "${ROOT}/cmake/NobionBuildInfoWrite.cmake"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "script failed for '${value}'")
  endif()
  file(READ "${WORK}/out.cpp" text)
  if(NOT text MATCHES "\n    \"1.2.3\",\n    \"([^\n]*)\",\n")
    message(FATAL_ERROR "no sha line for '${value}'")
  endif()
  if(NOT CMAKE_MATCH_1 STREQUAL expected)
    message(FATAL_ERROR "GITHUB_SHA='${value}': got '${CMAKE_MATCH_1}', want '${expected}'")
  endif()
endfunction()

sha_for("0123456789ABCDEFabcdef0123456789abcdef01" "0123456789ab")
sha_for("0123456789abcdef" "0123456789ab")
sha_for("abc123" "unknown")
sha_for("abcdef0123456789\"; evil" "unknown")
sha_for("abcdef0123456789 " "unknown")
sha_for("" "unknown")

# A source tree inside another repository takes that repository's sha only if
# it is that repository's root; otherwise GITHUB_SHA, or unknown.
find_program(GIT_FOR_TEST git)
if(GIT_FOR_TEST)
  set(repo "${WORK}/outer")
  file(REMOVE_RECURSE "${repo}")
  file(MAKE_DIRECTORY "${repo}/inner")
  file(WRITE "${repo}/inner/a.txt" "a\n")
  foreach(args "init" "add -A" "-c user.name=t -c user.email=t@t commit -m x")
    separate_arguments(argv UNIX_COMMAND "${args}")
    execute_process(COMMAND "${GIT_FOR_TEST}" ${argv} WORKING_DIRECTORY "${repo}"
      OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "git ${args} failed")
    endif()
  endforeach()
  execute_process(COMMAND "${GIT_FOR_TEST}" rev-parse HEAD WORKING_DIRECTORY "${repo}"
    OUTPUT_VARIABLE outer_head OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(SUBSTRING "${outer_head}" 0 12 outer_sha)
  sha_in("${repo}" "${GIT_FOR_TEST}" "" "${outer_sha}")
  sha_in("${repo}/inner" "${GIT_FOR_TEST}" "" "unknown")
  sha_in("${repo}/inner" "${GIT_FOR_TEST}" "0123456789abcdef" "0123456789ab")
endif()
