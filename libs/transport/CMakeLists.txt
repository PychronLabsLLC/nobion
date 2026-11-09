include(NobionAsio)

add_library(nobion_transport
  src/bytes.cpp
  src/read_spec.cpp
  src/health.cpp
  src/link_transport.cpp
  src/queued_transport.cpp
  src/sim_transport.cpp
  src/trace.cpp
  src/trace_recorder.cpp
  src/asio_stream.cpp
  src/serial_transport.cpp
  src/tcp_transport.cpp
  src/udp_transport.cpp
  src/factory.cpp
  src/listener.cpp
  src/endpoints.cpp
)
add_library(nobion::transport ALIAS nobion_transport)

target_include_directories(nobion_transport PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(nobion_transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nobion_transport PUBLIC cxx_std_20)
target_link_libraries(nobion_transport
  PUBLIC nobion::core Threads::Threads
  PRIVATE asio::asio)

if(WIN32)
  # asio pulls in <windows.h>; keep its min/max macros out of our code.
  target_compile_definitions(nobion_transport PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

nobion_set_warnings(nobion_transport)
