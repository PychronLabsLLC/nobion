# grains: labels grains in microscope video from Becke fringe motion. The Qt
# part over libs/grains; does not link pychron_ui_lib.
if(NOT TARGET pychron_grains)
  return()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)

# Everything except main() lives in a static library so tests can drive the
# same widgets under Qt Test.
add_library(pychron_grains_app_lib STATIC
  src/demo.cpp
  src/grain_overlay.cpp
  src/grains_window.cpp
)
target_include_directories(pychron_grains_app_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pychron_grains_app_lib PUBLIC pychron::grains Qt6::Widgets)
pychron_set_warnings(pychron_grains_app_lib)

add_executable(grains src/main.cpp)
target_link_libraries(grains PRIVATE pychron_grains_app_lib)
pychron_set_warnings(grains)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
