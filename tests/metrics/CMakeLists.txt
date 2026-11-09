add_executable(nobion_metrics_tests
  test_registry.cpp
  test_render.cpp
  test_server.cpp
  test_scheduler_metrics.cpp
  test_core_exporter.cpp
)

target_link_libraries(nobion_metrics_tests PRIVATE nobion::metrics asio::asio GTest::gtest_main)
# metrics_text.hpp is shared with the experiment and integration suites.
target_include_directories(nobion_metrics_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/support)
if(WIN32)
  target_compile_definitions(nobion_metrics_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
nobion_set_warnings(nobion_metrics_tests)

gtest_discover_tests(nobion_metrics_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
