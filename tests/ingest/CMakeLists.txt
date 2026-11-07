# One test_<component>.cpp per ingest component. The writer tests run against
# a real store, on the engines tests/persistence/store_fixture.hpp provides.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_INGEST_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(nobion_ingest_tests ${NOBION_INGEST_TESTS})
# store_fixture.hpp opens a white-box connection, so it sees TinyORM and Qt.
target_include_directories(nobion_ingest_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(nobion_ingest_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(nobion_ingest_tests PRIVATE nobion::ingest nobion::persistence nlohmann_json::nlohmann_json
  TinyOrm::TinyOrm Qt6::Core Qt6::Sql GTest::gtest_main)
nobion_set_warnings(nobion_ingest_tests)
gtest_discover_tests(nobion_ingest_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
