# pychron::grains: grain labelling from Becke fringe motion. No Qt.
if(NOT PYCHRON_VISION)
  return()
endif()

file(GLOB_RECURSE PYCHRON_GRAINS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(pychron_grains STATIC ${PYCHRON_GRAINS_SOURCES})
add_library(pychron::grains ALIAS pychron_grains)

target_include_directories(pychron_grains PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(pychron_grains PUBLIC cxx_std_20)
target_link_libraries(pychron_grains PUBLIC pychron::vision nlohmann_json::nlohmann_json)

pychron_set_warnings(pychron_grains)
