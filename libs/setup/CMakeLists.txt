# nobion::setup: install profiles, the site config and doctor (installation
# wizard spec). Qt-free; used by elctl init/doctor and the UI's setup wizard.
file(GLOB_RECURSE NOBION_SETUP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_setup STATIC ${NOBION_SETUP_SOURCES})
add_library(nobion::setup ALIAS nobion_setup)

target_include_directories(nobion_setup PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_setup PUBLIC cxx_std_20)
target_link_libraries(nobion_setup PUBLIC nobion::experiment nobion::systems nobion::core)
# miniz: the support bundle's zip (support_bundle.cpp), as libs/earthbank uses it.
target_link_libraries(nobion_setup PRIVATE miniz::miniz)

# The source tree: where a program that is not installed finds the profiles
# and example configs (setup::find_resources).
target_compile_definitions(nobion_setup PRIVATE NOBION_SETUP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

nobion_set_warnings(nobion_setup)
