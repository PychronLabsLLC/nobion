add_executable(nobion_transport_tests
  test_bytes.cpp
  test_read_spec.cpp
  test_queued_transport.cpp
  test_sim_transport.cpp
  test_trace.cpp
  test_trace_recorder.cpp
  test_transaction.cpp
  test_tcp_transport.cpp
  test_udp_transport.cpp
  test_serial_transport.cpp
  test_factory.cpp
  test_listener.cpp
  test_endpoints.cpp
)

target_link_libraries(nobion_transport_tests PRIVATE nobion::transport asio::asio GTest::gtest_main)
# virtual_time.hpp
target_include_directories(nobion_transport_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
if(WIN32)
  target_compile_definitions(nobion_transport_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
nobion_set_warnings(nobion_transport_tests)

gtest_discover_tests(nobion_transport_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
