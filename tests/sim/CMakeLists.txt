# One test_<component>.cpp per sim component. Globbed so parallel units only add files.
file(GLOB NOBION_SIM_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(NOBION_SIM_TESTS)
  add_executable(nobion_sim_tests ${NOBION_SIM_TESTS})
  target_link_libraries(nobion_sim_tests PRIVATE nobion::sim GTest::gtest_main)
  # virtual_time.hpp
  target_include_directories(nobion_sim_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(nobion_sim_tests PRIVATE
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
  # sim drivers register from static initializers nothing references by symbol
  nobion_link_drivers(nobion_sim_tests nobion_sim)
  nobion_set_warnings(nobion_sim_tests)
  gtest_discover_tests(nobion_sim_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
