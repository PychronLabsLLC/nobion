# One test_<component>.cpp per update component. Globbed so parallel units
# only add files. Fixtures are GitHub answers kept in fixtures/.
file(GLOB NOBION_UPDATE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# FakeHttp, release_json() and the fixture directory, for tests that fake the
# update check's HTTP seam. The elctl and nobion tests link this too.
add_library(nobion_update_testing INTERFACE)
add_library(nobion::update_testing ALIAS nobion_update_testing)
target_include_directories(nobion_update_testing INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nobion_update_testing INTERFACE
  NOBION_UPDATE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(nobion_update_testing INTERFACE nobion::update GTest::gtest)

add_executable(nobion_update_tests ${NOBION_UPDATE_TESTS})
target_link_libraries(nobion_update_tests PRIVATE nobion::update_testing GTest::gtest_main)
nobion_set_warnings(nobion_update_tests)
gtest_discover_tests(nobion_update_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
