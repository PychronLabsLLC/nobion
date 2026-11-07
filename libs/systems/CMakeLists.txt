# Managers and facades built on capability interfaces: SwitchManager,
# GaugeScanner, NetworkGraph/canvas model, ExtractionLine facade, and later
# the Spectrometer. Vendor-blind: depends on nobion::devices interfaces only.
#
# Sources are globbed so units landing in parallel only add files under
# src/ and include/nobion/systems/ and never edit this list.
file(GLOB_RECURSE NOBION_SYSTEMS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_systems STATIC ${NOBION_SYSTEMS_SOURCES})
add_library(nobion::systems ALIAS nobion_systems)

target_include_directories(nobion_systems PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_systems PUBLIC cxx_std_20)
# nobion::sim backs `kind = "sim"` transports in the ExtractionLine facade.
target_link_libraries(nobion_systems PUBLIC nobion::sim nobion::devices nobion::transport nobion::core)

nobion_set_warnings(nobion_systems)
