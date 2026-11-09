# One test_<component>.cpp per dvc component. The layout tests read the real
# legacy files under fixtures/ (see fixtures/README.md) and need no store; the
# import tests run on the engines tests/persistence/store_fixture.hpp provides.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_DVC_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

add_executable(nobion_dvc_tests ${NOBION_DVC_TESTS})
# test_legacy_json.cpp tests the private reader, so it sees src/ and the JSON
# library. test_project_import.cpp writes to a real store through
# tests/persistence/store_fixture.hpp, which opens a white-box connection and
# so sees TinyORM and Qt.
target_include_directories(nobion_dvc_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/libs/dvc/src"
  "${PROJECT_SOURCE_DIR}/tests/persistence"
  "${PROJECT_SOURCE_DIR}/libs/persistence/src")
target_compile_definitions(nobion_dvc_tests PRIVATE
  NOBION_DVC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures" QT_NO_KEYWORDS)
target_link_libraries(nobion_dvc_tests PRIVATE nobion::dvc nobion::ingest nobion::persistence
  nlohmann_json::nlohmann_json TinyOrm::TinyOrm Qt6::Core Qt6::Sql GTest::gtest_main)
nobion_set_warnings(nobion_dvc_tests)
gtest_discover_tests(nobion_dvc_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
