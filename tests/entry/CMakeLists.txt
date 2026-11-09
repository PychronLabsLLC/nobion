# One test_<unit>.cpp per entry unit. The tests that write go through a real
# store, on the engines tests/persistence/store_fixture.hpp provides.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_ENTRY_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(nobion_entry_tests ${NOBION_ENTRY_TESTS})
# store_fixture.hpp opens a white-box connection, so it sees TinyORM and Qt.
target_include_directories(nobion_entry_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
# test_seed.cpp reads the seed an install ships.
target_compile_definitions(nobion_entry_tests PRIVATE QT_NO_KEYWORDS
  NOBION_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
target_link_libraries(nobion_entry_tests PRIVATE nobion::entry nobion::persistence TinyOrm::TinyOrm Qt6::Core
  Qt6::Sql GTest::gtest_main)
nobion_set_warnings(nobion_entry_tests)
gtest_discover_tests(nobion_entry_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
