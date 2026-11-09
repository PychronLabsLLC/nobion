include(NobionAsio)

# nobion::metrics: a registry of counters, gauges and histograms, their
# Prometheus text rendering and the HTTP endpoint that serves it. asio is a
# PRIVATE dependency: no public metrics header includes it.
add_library(nobion_metrics
  src/registry.cpp
  src/render.cpp
  src/server.cpp
  src/scheduler_metrics.cpp
  src/core_exporter.cpp
)
add_library(nobion::metrics ALIAS nobion_metrics)

target_include_directories(nobion_metrics PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(nobion_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nobion_metrics PUBLIC cxx_std_20)
target_link_libraries(nobion_metrics
  PUBLIC nobion::core Threads::Threads
  PRIVATE asio::asio)

if(WIN32)
  # asio pulls in <windows.h>; keep its min/max macros out of our code.
  target_compile_definitions(nobion_metrics PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

nobion_set_warnings(nobion_metrics)
