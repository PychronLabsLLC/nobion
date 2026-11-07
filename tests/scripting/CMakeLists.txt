# One test_<component>.cpp per scripting component. Globbed so parallel units only add files.
file(GLOB NOBION_SCRIPTING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(NOBION_SCRIPTING_TESTS)
  add_executable(nobion_scripting_tests ${NOBION_SCRIPTING_TESTS})
  target_link_libraries(nobion_scripting_tests PRIVATE nobion::scripting GTest::gtest_main)
  target_include_directories(nobion_scripting_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support
    ${PROJECT_SOURCE_DIR}/libs/scripting/src/python)
  target_compile_definitions(nobion_scripting_tests PRIVATE
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    NOBION_SCRIPTING_TEST_SCRIPTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts")
  nobion_set_warnings(nobion_scripting_tests)
  gtest_discover_tests(nobion_scripting_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
