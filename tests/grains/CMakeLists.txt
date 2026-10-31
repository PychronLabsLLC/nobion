# One test_<component>.cpp per grains component.
if(NOT PYCHRON_VISION)
  return()
endif()

file(GLOB PYCHRON_GRAINS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(PYCHRON_GRAINS_TESTS)
  add_executable(pychron_grains_tests ${PYCHRON_GRAINS_TESTS})
  target_link_libraries(pychron_grains_tests PRIVATE pychron::grains GTest::gtest_main)
  pychron_set_warnings(pychron_grains_tests)
  gtest_discover_tests(pychron_grains_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
