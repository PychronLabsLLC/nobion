# nobion::sim. Sources are globbed so units landing in parallel only add
# files under src/ and include/nobion/sim/ and never edit this list.
file(GLOB_RECURSE NOBION_SIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_sim STATIC ${NOBION_SIM_SOURCES})
add_library(nobion::sim ALIAS nobion_sim)

target_include_directories(nobion_sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_sim PUBLIC cxx_std_20)
# sim_config.cpp reads sim.toml with toml++, which nobion::core links PUBLIC.
target_link_libraries(nobion_sim PUBLIC nobion::devices nobion::transport nobion::core)

nobion_set_warnings(nobion_sim)
