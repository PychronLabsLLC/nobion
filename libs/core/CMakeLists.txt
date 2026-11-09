find_package(Threads REQUIRED)

add_library(nobion_core
  src/error.cpp
  src/device_state.cpp
  src/clock.cpp
  src/virtual_clock.cpp
  src/clock_mutex.cpp
  src/time_series_ring.cpp
  src/signal_bus.cpp
  src/logger.cpp
  src/log_context.cpp
  src/log_error.cpp
  src/log_summary.cpp
  src/redact.cpp
  src/log_match.cpp
  src/log_hub.cpp
  src/byte_ring.cpp
  src/scheduler.cpp
  src/host_name.cpp
  src/process.cpp
  src/sha256.cpp
  src/sha1.cpp
  src/calendar.cpp
  src/user_file.cpp
  src/atomic_file.cpp
  src/config/diagnostic.cpp
  src/config/loader.cpp
  src/config/logging_file.cpp
  src/log_dir.cpp
  src/process_log.cpp
  src/event_recorder.cpp
  src/config/local_file.cpp
  src/config/validate.cpp
  src/toml_impl.cpp
  src/build_info_id.cpp
  src/signal_stack.cpp
  src/module_map.cpp
  src/flight_recorder_triggers.cpp
  src/hang_monitor.cpp
)
nobion_generate_build_info(nobion_core)
add_library(nobion::core ALIAS nobion_core)

target_include_directories(nobion_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_core PUBLIC cxx_std_20)

# toml::table appears in public config headers (DriverConfig::options), so the
# toml++ configuration must match in every translation unit: no exceptions.
# TOML_FLOAT_CHARCONV=0: toml++ writes a float with std::to_chars where it
# trusts the compiler's (MSVC: shortest form, 0.1) and through a stream at 17
# digits elsewhere (0.10000000000000001). A record's bytes, and so its sha,
# must not depend on what built the program: the stream everywhere.
# TOML_HEADER_ONLY=0: toml++ is compiled once, in src/toml_impl.cpp.
target_compile_definitions(nobion_core PUBLIC TOML_EXCEPTIONS=0 TOML_FLOAT_CHARCONV=0 TOML_HEADER_ONLY=0)
target_link_libraries(nobion_core PUBLIC tomlplusplus::tomlplusplus Threads::Threads)
# spdlog is an implementation detail of LogHub: never exposed (spec 4.6).
target_link_libraries(nobion_core PRIVATE spdlog::spdlog)
# MiniDumpWriteDump: the crash handler's minidump on Windows (log_hub.cpp).
if(WIN32)
  target_link_libraries(nobion_core PRIVATE dbghelp)
endif()

nobion_set_warnings(nobion_core)
