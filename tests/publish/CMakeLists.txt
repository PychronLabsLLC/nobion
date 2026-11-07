# One test_<component>.cpp per publish component, plus the elctl publish and
# embargo commands (the first tests to drive nobion_elctl_lib). The release
# fixture is a store (tests/persistence/store_fixture.hpp, which needs Qt and
# TinyORM): SQLite always, PostgreSQL as well when NOBION_TEST_PG_URL is set.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_PUBLISH_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(nobion_publish_tests ${NOBION_PUBLISH_TESTS})
target_include_directories(nobion_publish_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(nobion_publish_tests PRIVATE QT_NO_KEYWORDS
  NOBION_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  NOBION_PUBLISH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# arar_store: the Ar-Ar loader the release's store source opens with.
target_link_libraries(nobion_publish_tests PRIVATE nobion::publish nobion::arar nobion::arar_store nobion::processing
  nobion::reduction nobion_elctl_lib TinyOrm::TinyOrm Qt6::Core Qt6::Sql GTest::gtest_main)
nobion_set_warnings(nobion_publish_tests)
# test_parquet.cpp reads the files back with Arrow when the build has it.
if(NOBION_PUBLISH_ARROW_ENABLED)
  target_link_libraries(nobion_publish_tests PRIVATE ${NOBION_PUBLISH_ARROW_LIBS})
  target_compile_definitions(nobion_publish_tests PRIVATE NOBION_PUBLISH_ARROW_ENABLED)
endif()
gtest_discover_tests(nobion_publish_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
