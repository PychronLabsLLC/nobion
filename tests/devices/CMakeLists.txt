# A driver library nothing references by symbol, linked the way drivers are.
add_library(nobion_devices_test_drivers STATIC archived_driver.cpp)
target_link_libraries(nobion_devices_test_drivers PRIVATE nobion::devices)
nobion_set_warnings(nobion_devices_test_drivers)

add_executable(nobion_devices_tests
  test_device.cpp
  test_capabilities.cpp
  test_driver_registry.cpp
  test_link.cpp
  test_repeat_tables.cpp
  test_modbus_ids.cpp
  test_codec_conventions.cpp
  test_pfeiffer_maxigauge.cpp
  test_gp_microion.cpp
  test_proxr_relay.cpp
  test_proxr_board_sim.cpp
  test_sim_valves.cpp
  test_valve_conformance.cpp
  test_agilent_switch.cpp
  test_pychron_valves.cpp
  test_varian_xgs600.cpp
  test_plc2000_gauges.cpp
  test_plc2000_heater.cpp
  test_plc2000_valves.cpp
  test_lakeshore.cpp
  spectrometer/test_roles.cpp
  spectrometer/test_params.cpp
  spectrometer/test_detectors.cpp
  spectrometer/test_conformance_fakes.cpp
  spectrometer/test_thermo_qtegra.cpp
  spectrometer/test_thermo_qtegra_acquire.cpp
  spectrometer/test_thermo_qtegra_conformance.cpp
  spectrometer/test_ngx.cpp
  spectrometer/test_qtegra_link.cpp
  spectrometer/test_qtegra_valves.cpp
  spectrometer/test_qtegra_gauges.cpp
  spectrometer/legacy/test_dac_positioner.cpp
  spectrometer/legacy/test_serial_hv.cpp
  spectrometer/legacy/test_acquirers.cpp
  spectrometer/legacy/test_legacy_conformance.cpp
  spectrometer/legacy/test_sim_legacy_config.cpp
  extraction/test_capability.cpp
  extraction/test_conformance_fake.cpp
  extraction/test_chromium_sim.cpp
  extraction/test_chromium.cpp
)
# spectrometer/ and extraction/ conformance.hpp and fakes
target_include_directories(nobion_devices_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# virtual_time.hpp
target_include_directories(nobion_devices_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)

# test_pychron_valves runs a loopback TCP server.
target_link_libraries(nobion_devices_tests PRIVATE nobion::devices asio::asio GTest::gtest_main)
# Committed wire traces, replayed by driver tests.
target_compile_definitions(nobion_devices_tests PRIVATE
  NOBION_TRACES_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
nobion_link_drivers(nobion_devices_tests nobion_devices_test_drivers)
# Example configs, driven end to end by spectrometer/legacy.
target_compile_definitions(nobion_devices_tests PRIVATE
  NOBION_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/configs/examples")
target_compile_definitions(nobion_devices_tests PRIVATE
  NOBION_TRACE_DIR="${PROJECT_SOURCE_DIR}/tests/traces")
nobion_set_warnings(nobion_devices_tests)

gtest_discover_tests(nobion_devices_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
