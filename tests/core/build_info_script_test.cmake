# ctest driver: runs cmake/NobionBuildInfoWrite.cmake with no git and a given
# GITHUB_SHA, and checks the sha it writes. Inputs: ROOT, WORK.
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/features.txt" "metrics\n")

function(sha_for value expected)
  file(REMOVE "${WORK}/out.cpp")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "GITHUB_SHA=${value}" "${CMAKE_COMMAND}"
      -DSOURCE_DIR=${WORK} -DGIT_EXECUTABLE= -DVERSION=1.2.3
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
