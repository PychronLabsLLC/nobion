# Build identity (docs/superpowers/specs/2026-11-06-field-diagnostics-design.md
# section 2): build_info.cpp, generated in the build tree, names the version,
# the git commit, whether the tree was dirty and the optional parts built in.
#
# nobion_generate_build_info(<target>) adds the generated source to <target>.
# It is written at build time by cmake/NobionBuildInfoWrite.cmake, which runs
# again when the git dir's HEAD or index changes (a commit, a checkout, a
# refreshed index), and writes the file only when its content differs, so
# nothing is recompiled or relinked otherwise.

set(NOBION_BUILD_INFO_DIR "${CMAKE_SOURCE_DIR}")

function(nobion_generate_build_info target)
  set(out "${CMAKE_BINARY_DIR}/generated/build_info.cpp")
  set(features_file "${CMAKE_BINARY_DIR}/generated/build_features.txt")

  # In a worktree .git is a file: ask git, and make the answer absolute.
  set(git_deps "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_rc ERROR_QUIET)
    if(git_rc EQUAL 0 AND IS_DIRECTORY "${git_dir}")
      set(git_deps "${git_dir}/HEAD" "${git_dir}/index")
    endif()
  endif()

  # Features known at configure time here; whether libs/persistence exists is
  # only known once every directory has been read, so that one is a generator
  # expression, evaluated when the build system is written.
  set(features "")
  if(NOBION_SCRIPTING_ENABLED)
    string(APPEND features "scripting\n")
  endif()
  if(NOBION_VISION_OPENCV_ENABLED)
    string(APPEND features "vision_opencv\n")
  endif()
  if(NOBION_PUBLISH_ARROW_ENABLED)
    string(APPEND features "publish_parquet\n")
  endif()
  if(NOBION_METRICS)
    string(APPEND features "metrics\n")
  endif()
  # NOBION_HARDENED_STDLIB is set by the hardened-library option; unset is off.
  if(NOBION_HARDENED_STDLIB)
    string(APPEND features "hardened_stdlib\n")
  endif()
  file(GENERATE OUTPUT "${features_file}"
    CONTENT "${features}$<$<TARGET_EXISTS:nobion_persistence>:persistence\n>")

  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${CMAKE_COMMAND}"
      -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
      -DVERSION=${PROJECT_VERSION}
      -DTEMPLATE=${CMAKE_SOURCE_DIR}/cmake/build_info.cpp.in
      -DFEATURES_FILE=${features_file}
      -DOUT=${out}
      -P "${CMAKE_SOURCE_DIR}/cmake/NobionBuildInfoWrite.cmake"
    DEPENDS ${git_deps} "${features_file}"
      "${CMAKE_SOURCE_DIR}/cmake/build_info.cpp.in"
      "${CMAKE_SOURCE_DIR}/cmake/NobionBuildInfoWrite.cmake"
    COMMENT "Generating build_info.cpp"
    VERBATIM)
  set_source_files_properties("${out}" PROPERTIES
    GENERATED TRUE
    COMPILE_DEFINITIONS "NOBION_BUILD_TYPE=\"$<CONFIG>\"")
  target_sources(${target} PRIVATE "${out}")
endfunction()
