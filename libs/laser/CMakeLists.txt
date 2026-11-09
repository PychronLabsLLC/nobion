# The laser system: tray maps, stage calibration and the facade that turns a
# hole name into stage millimetres for any extraction-device driver
# (docs/superpowers/specs/2026-10-04-laser-system-design.md). No Qt, no OpenCV.
file(GLOB NOBION_LASER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_laser STATIC ${NOBION_LASER_SOURCES})
add_library(nobion::laser ALIAS nobion_laser)

target_include_directories(nobion_laser PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_laser PUBLIC cxx_std_20)
target_link_libraries(nobion_laser PUBLIC nobion::devices nobion::vision nobion::core)

nobion_set_warnings(nobion_laser)
