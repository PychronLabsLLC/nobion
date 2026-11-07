# The Ar-Ar suites: reduction step functions, reduce(), decay, flux and the
# golden vectors under golden/ (spec 9.4); the figures, the report, group
# ages, the flux fit and view, and the domain. Globbed so parallel units only
# add files. The store suites (the flux store, the store source's Ar-Ar
# loader) need the persistence library and build only when the store adapter
# exists; the records loader suite, only when the records adapter does.
file(GLOB NOBION_ARAR_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
list(FILTER NOBION_ARAR_TESTS EXCLUDE REGEX "test_(flux_store|store_loader|store_load_timing|record_loader)\\.cpp$")

if(NOBION_ARAR_TESTS)
  add_executable(nobion_arar_tests ${NOBION_ARAR_TESTS})
  target_link_libraries(nobion_arar_tests PRIVATE nobion::arar GTest::gtest_main)
  # tests/support: golden_json.hpp, the golden reader shared with
  # tests/reduction; tests/processing: fixtures.hpp, the analyses the
  # processing suites are built on (arar_fixtures.hpp adds their Ar-Ar data).
  target_include_directories(nobion_arar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support
    ${PROJECT_SOURCE_DIR}/tests/processing)
  target_compile_definitions(nobion_arar_tests PRIVATE
    NOBION_REDUCTION_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
  nobion_set_warnings(nobion_arar_tests)
  # -ffp-contract=off (unfused test arithmetic) comes PUBLIC from nobion::arar_reduction.
  gtest_discover_tests(nobion_arar_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET nobion_arar_store)
  add_executable(nobion_arar_store_tests test_flux_store.cpp test_store_loader.cpp test_store_load_timing.cpp)
  # The flux store tests run on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  # tests/processing: store_source_fixture.hpp, the store the store source
  # suites share. fixtures/: the input fingerprint recorded before the domain
  # seam moved the Ar-Ar inputs behind the loader.
  target_include_directories(nobion_arar_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src"
    "${PROJECT_SOURCE_DIR}/tests/processing")
  target_compile_definitions(nobion_arar_store_tests PRIVATE QT_NO_KEYWORDS
    NOBION_ARAR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  target_link_libraries(nobion_arar_store_tests PRIVATE nobion::arar_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  nobion_set_warnings(nobion_arar_store_tests)
  gtest_discover_tests(nobion_arar_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()

if(TARGET nobion_arar_records)
  add_executable(nobion_arar_records_tests test_record_loader.cpp)
  # tests/processing: record_fixtures.hpp, the records the record source suites write.
  target_include_directories(nobion_arar_records_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/processing")
  target_link_libraries(nobion_arar_records_tests PRIVATE nobion::arar_records GTest::gtest_main)
  nobion_set_warnings(nobion_arar_records_tests)
  gtest_discover_tests(nobion_arar_records_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
