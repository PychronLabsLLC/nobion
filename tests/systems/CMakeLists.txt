# One test_<component>.cpp per systems component. Globbed so parallel units
# only add files here.
file(GLOB NOBION_SYSTEMS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(NOBION_SYSTEMS_TESTS)
  add_executable(nobion_systems_tests ${NOBION_SYSTEMS_TESTS})
  target_link_libraries(nobion_systems_tests PRIVATE nobion::systems GTest::gtest_main)
  # virtual_time.hpp
  target_include_directories(nobion_systems_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(nobion_systems_tests PRIVATE
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    NOBION_SYSTEMS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  # spectrometer sim drivers register from static initializers nothing references
  nobion_link_drivers(nobion_systems_tests nobion_sim)
  nobion_set_warnings(nobion_systems_tests)
  gtest_discover_tests(nobion_systems_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 180)
endif()
