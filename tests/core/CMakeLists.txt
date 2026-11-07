add_executable(nobion_core_tests
  test_expected.cpp
  test_error.cpp
  test_clock.cpp
  test_virtual_clock.cpp
  test_clock_mutex.cpp
  test_time_series_ring.cpp
  test_signal_bus.cpp
  test_logger.cpp
  test_log_match.cpp
  test_log_hub.cpp
  test_byte_ring.cpp
  test_log_context.cpp
  test_log_error.cpp
  test_log_summary.cpp
  test_scheduler.cpp
  test_host_name.cpp
  test_process.cpp
  test_user_file.cpp
  test_atomic_file.cpp
  test_number.cpp
  test_path_text.cpp
  test_sha256.cpp
  test_sha1.cpp
  test_calendar.cpp
  test_config_loader.cpp
  test_config_validate.cpp
  test_config_override.cpp
  test_local_file.cpp
  test_logging_file.cpp
  test_log_dir.cpp
  test_process_log.cpp
  test_event_recorder.cpp
  test_http.cpp
  test_keyed_noise.cpp
  test_build_info.cpp
  test_crash_capture.cpp
)

# Dies on purpose, in the way its second argument names; test_crash_capture.cpp
# spawns it and reads the crash file it leaves.
add_executable(nobion_crash_helper crash_helper.cpp)
target_link_libraries(nobion_crash_helper PRIVATE nobion::core)
nobion_set_warnings(nobion_crash_helper)

target_link_libraries(nobion_core_tests PRIVATE nobion::core GTest::gtest_main)
# virtual_time.hpp; byte_ring.hpp (internal to libs/core)
target_include_directories(nobion_core_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support
  ${PROJECT_SOURCE_DIR}/libs/core/src)
nobion_set_warnings(nobion_core_tests)
target_compile_definitions(nobion_core_tests PRIVATE
  NOBION_TEST_PROJECT_VERSION="${PROJECT_VERSION}"
  NOBION_TEST_HAS_METRICS=$<BOOL:${NOBION_METRICS}>
  NOBION_CRASH_HELPER="$<TARGET_FILE:nobion_crash_helper>")
add_dependencies(nobion_core_tests nobion_crash_helper)

gtest_discover_tests(nobion_core_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS release_config TIMEOUT 60)

# The sha the build-info script writes when git gives none (CI's GITHUB_SHA).
add_test(NAME BuildInfoScript.GithubShaFallback
  COMMAND "${CMAKE_COMMAND}" -DROOT=${PROJECT_SOURCE_DIR} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build_info_script
    -P "${CMAKE_CURRENT_SOURCE_DIR}/build_info_script_test.cmake")
set_tests_properties(BuildInfoScript.GithubShaFallback PROPERTIES TIMEOUT 60)

# tools/quality_check.py (static analysis of a branch's changes) is tested with
# the standard library's unittest; the analysers themselves are not run.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME tools.quality_check
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_quality_check.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.quality_check PROPERTIES TIMEOUT 60)
  # tools/check_atomic_writes.py: a rename( outside write_file_atomic is reported.
  add_test(NAME tools.check_atomic_writes
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_check_atomic_writes.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.check_atomic_writes PROPERTIES TIMEOUT 60)
  # tools/compiler_sweep.py --changed: which sources a branch's changes reach,
  # from Ninja's dependency log; no compiler is run.
  add_test(NAME tools.compiler_sweep
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_compiler_sweep.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.compiler_sweep PROPERTIES TIMEOUT 60)
  # tools/coverage_report.py: which binaries are objects, what of llvm-cov's
  # summary is kept, what counts as a drop; llvm-cov itself is not run.
  add_test(NAME tools.coverage_report
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_coverage_report.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.coverage_report PROPERTIES TIMEOUT 60)
  # tools/symbolize.py and tools/split_symbols.py: crash file parsing, the call
  # made per frame (stand-ins for atos and addr2line), the archive names.
  add_test(NAME tools.symbolize
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_symbolize.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.symbolize PROPERTIES TIMEOUT 60)
  # tools/agent_bench (the agent toolchain's ledger and fixed set): its tests
  # start no agent; the runner is given a repository made in a temporary
  # directory and a stand-in for the agent.
  add_test(NAME tools.agent_bench
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -p "test_agent_bench.py"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.agent_bench PROPERTIES TIMEOUT 120)
endif()
