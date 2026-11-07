# One test_<component>.cpp per processing component. The record-source suite
# and the store-source suite need the experiment and persistence libraries and
# build only when their adapters exist.
file(GLOB NOBION_PROCESSING_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER NOBION_PROCESSING_TESTS EXCLUDE REGEX "test_(record_source|store_source)\\.cpp$")

add_executable(nobion_processing_tests ${NOBION_PROCESSING_TESTS})
target_include_directories(nobion_processing_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(nobion_processing_tests PRIVATE nobion::processing GTest::gtest_main)
# The committed SVG and PDF of a scene (fixtures/scene): a stored format, so the
# bytes one compiler writes are checked under every other (root AGENTS.md).
target_compile_definitions(nobion_processing_tests PRIVATE
  NOBION_PROCESSING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
nobion_set_warnings(nobion_processing_tests)
gtest_discover_tests(nobion_processing_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS release_config TIMEOUT 60)

if(TARGET nobion_processing_records)
  add_executable(nobion_processing_records_tests test_record_source.cpp)
  target_link_libraries(nobion_processing_records_tests PRIVATE nobion::processing_records GTest::gtest_main)
  nobion_set_warnings(nobion_processing_records_tests)
  gtest_discover_tests(nobion_processing_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS release_config TIMEOUT 60)
endif()

if(TARGET nobion_processing_store)
  add_executable(nobion_processing_store_tests test_store_source.cpp)
  # The store tests run on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  target_include_directories(nobion_processing_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(nobion_processing_store_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(nobion_processing_store_tests PRIVATE nobion::processing_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  nobion_set_warnings(nobion_processing_store_tests)
  gtest_discover_tests(nobion_processing_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS release_config TIMEOUT 120)
endif()
