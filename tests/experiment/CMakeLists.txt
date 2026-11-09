# One test_<component>.cpp per experiment component. Globbed so parallel units only add files.
file(GLOB NOBION_EXPERIMENT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# The record-ingest suite needs the store adapter and builds only with it.
list(FILTER NOBION_EXPERIMENT_TESTS EXCLUDE REGEX "test_(record_ingest|store_persister)\\.cpp$")

if(NOT TARGET nobion_metrics)
  list(FILTER NOBION_EXPERIMENT_TESTS EXCLUDE REGEX "test_(experiment_metrics|metrics_service)\\.cpp$")
endif()

if(NOBION_EXPERIMENT_TESTS)
  add_executable(nobion_experiment_tests ${NOBION_EXPERIMENT_TESTS})
  target_link_libraries(nobion_experiment_tests PRIVATE nobion::experiment GTest::gtest_main)
  target_compile_definitions(nobion_experiment_tests PRIVATE
    NOBION_EXAMPLE_CONFIGS_DIR="${PROJECT_SOURCE_DIR}/configs/examples"
    NOBION_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data"
    NOBION_PLAN_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plans")
  # virtual_time.hpp; plan/plan_parse.hpp (the plan loader's ungated resolve)
  target_include_directories(nobion_experiment_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support
    ${PROJECT_SOURCE_DIR}/libs/experiment/src)
  if(TARGET nobion_metrics)
    # test_metrics_service.cpp scrapes over a socket (http_client.hpp)
    target_link_libraries(nobion_experiment_tests PRIVATE asio::asio)
    if(WIN32)
      target_compile_definitions(nobion_experiment_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
  endif()
  nobion_set_warnings(nobion_experiment_tests)
  gtest_discover_tests(nobion_experiment_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()

if(TARGET nobion_experiment_store)
  add_executable(nobion_experiment_store_tests test_record_ingest.cpp test_store_persister.cpp)
  target_include_directories(nobion_experiment_store_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
  # The persister suite runs on every engine the persistence tests do; their
  # database fixture (tests/persistence/store_fixture.hpp) makes a PostgreSQL
  # schema through persistence's own connection, so sees TinyORM and Qt.
  target_include_directories(nobion_experiment_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${PROJECT_SOURCE_DIR}/tests/persistence" "${PROJECT_SOURCE_DIR}/libs/persistence/src")
  target_compile_definitions(nobion_experiment_store_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(nobion_experiment_store_tests PRIVATE nobion::experiment_store TinyOrm::TinyOrm Qt6::Core
    Qt6::Sql nlohmann_json::nlohmann_json GTest::gtest_main)
  nobion_set_warnings(nobion_experiment_store_tests)
  gtest_discover_tests(nobion_experiment_store_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endif()
