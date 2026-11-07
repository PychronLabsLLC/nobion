# One test_<component>.cpp per persistence component. PostgreSQL cases run only
# when NOBION_TEST_PG_URL is set (e.g. postgresql://user:pw@localhost/nobion_test);
# SQLite cases always run.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_PERSISTENCE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(nobion_persistence_tests ${NOBION_PERSISTENCE_TESTS})
# White-box tests (DDL parity, triggers, statement splitting) reach into src/
# and therefore see TinyORM and Qt.
target_include_directories(nobion_persistence_tests PRIVATE "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(nobion_persistence_tests PRIVATE QT_NO_KEYWORDS)
target_link_libraries(nobion_persistence_tests PRIVATE nobion::persistence TinyOrm::TinyOrm Qt6::Core Qt6::Sql
  GTest::gtest_main)
nobion_set_warnings(nobion_persistence_tests)
gtest_discover_tests(nobion_persistence_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
