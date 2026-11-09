# End-to-end: example configs with kind = "sim" -> ExtractionLine ->
# SimSystem -> actuate -> assert pressures; the measurement path over the sim
# line and spectrometer. Globbed so later suites only add files.
file(GLOB NOBION_INTEGRATION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
# The install end-to-end test needs the store and has its own executable (below).
list(FILTER NOBION_INTEGRATION_TESTS EXCLUDE REGEX "test_(install_e2e|outage)\\.cpp$")

if(NOBION_INTEGRATION_TESTS)
  add_executable(nobion_integration_tests ${NOBION_INTEGRATION_TESTS})
  target_link_libraries(nobion_integration_tests PRIVATE nobion::experiment nobion::systems GTest::gtest_main)
  target_include_directories(nobion_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  target_compile_definitions(nobion_integration_tests PRIVATE
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_PLAN_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/experiment/fixtures/plans/valid"
    NOBION_INTEGRATION_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  if(TARGET nobion_metrics)
    # test_metrics_packaging.cpp reads packaging/observability; the metrics
    # tests scrape over a socket (http_client.hpp).
    target_compile_definitions(nobion_integration_tests PRIVATE NOBION_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(nobion_integration_tests PRIVATE asio::asio)
    if(WIN32)
      target_compile_definitions(nobion_integration_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    # The dashboards are checked as JSON where a JSON library is built (with persistence).
    if(TARGET nlohmann_json::nlohmann_json)
      target_link_libraries(nobion_integration_tests PRIVATE nlohmann_json::nlohmann_json)
      target_compile_definitions(nobion_integration_tests PRIVATE NOBION_TEST_HAS_JSON=1)
    endif()
  endif()
  # spectrometer sim drivers register from static initializers nothing references
  nobion_link_drivers(nobion_integration_tests nobion_sim)
  nobion_set_warnings(nobion_integration_tests)
  gtest_discover_tests(nobion_integration_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 180)
endif()

# A fresh install's queue, run on simulated hardware into the install's
# database and read back as the data window reads it. Its database fixture
# (tests/persistence/store_fixture.hpp) makes a PostgreSQL schema through
# persistence's own connection, so sees TinyORM and Qt.
if(TARGET nobion_experiment_store AND TARGET nobion_processing_store AND TARGET nobion_processing_records
   AND TARGET nobion_entry)
  add_executable(nobion_install_e2e_tests test_install_e2e.cpp)
  target_include_directories(nobion_install_e2e_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(nobion_install_e2e_tests PRIVATE QT_NO_KEYWORDS
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")
  target_link_libraries(nobion_install_e2e_tests PRIVATE nobion::experiment_store nobion::processing_store
    nobion::processing_records nobion::entry nobion::setup nobion::experiment nobion::systems TinyOrm::TinyOrm
    Qt6::Core Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  # the spectrometer sim drivers the install's simulation mode names
  nobion_link_drivers(nobion_install_e2e_tests nobion_sim)
  nobion_set_warnings(nobion_install_e2e_tests)
  gtest_discover_tests(nobion_install_e2e_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()

# An outage at every instant of the example queue (executor hardening): some
# hundred queues, each a second or so of simulated hardware. Its own
# executable for a timeout fit for it; dev-quick leaves it out.
add_executable(nobion_outage_tests test_outage.cpp)
target_link_libraries(nobion_outage_tests PRIVATE nobion::experiment nobion::systems GTest::gtest_main)
target_include_directories(nobion_outage_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
target_compile_definitions(nobion_outage_tests PRIVATE
  NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
nobion_link_drivers(nobion_outage_tests nobion_sim)
nobion_set_warnings(nobion_outage_tests)
# Above the fixture's dead-man (600 s), as AGENTS.md "Tests" asks.
gtest_discover_tests(nobion_outage_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 900)
