# One test_<component>.cpp per reduction component. Globbed so parallel units only add files.
file(GLOB NOBION_REDUCTION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(NOBION_REDUCTION_TESTS)
  add_executable(nobion_reduction_tests ${NOBION_REDUCTION_TESTS})
  target_link_libraries(nobion_reduction_tests PRIVATE nobion::reduction GTest::gtest_main)
  # golden_json.hpp, the golden reader shared with tests/arar.
  target_include_directories(nobion_reduction_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # The golden vectors are one directory (tools/reduction_golden/generate.py
  # --out): test_ufloat.cpp reads ufloat.json from it.
  target_compile_definitions(nobion_reduction_tests PRIVATE
    NOBION_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    NOBION_REDUCTION_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/arar/golden")
  nobion_set_warnings(nobion_reduction_tests)
  # -ffp-contract=off (unfused test arithmetic) comes PUBLIC from nobion::reduction.
  gtest_discover_tests(nobion_reduction_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
