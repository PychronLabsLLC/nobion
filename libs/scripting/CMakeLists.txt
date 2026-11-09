# nobion::scripting. Sources are globbed so units landing in parallel only add
# files under src/ and include/nobion/scripting/ and never edit this list.
# src/python/ holds the embedded-CPython host (NOBION_SCRIPTING); src/stub/
# the host that rejects scripted runs when scripting is off.
file(GLOB_RECURSE NOBION_SCRIPTING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(NOBION_SCRIPTING_ENABLED)
  list(FILTER NOBION_SCRIPTING_SOURCES EXCLUDE REGEX "/src/stub/")
else()
  list(FILTER NOBION_SCRIPTING_SOURCES EXCLUDE REGEX "/src/python/")
endif()

add_library(nobion_scripting STATIC ${NOBION_SCRIPTING_SOURCES})
add_library(nobion::scripting ALIAS nobion_scripting)

target_include_directories(nobion_scripting PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_scripting PUBLIC cxx_std_20)
target_link_libraries(nobion_scripting PUBLIC nobion::devices nobion::core)

if(NOBION_SCRIPTING_ENABLED)
  target_link_libraries(nobion_scripting PRIVATE pybind11::embed)
  target_compile_definitions(nobion_scripting PUBLIC NOBION_SCRIPTING_ENABLED=1)
  # pybind11 requires hidden visibility for its own types.
  set_target_properties(nobion_scripting PROPERTIES CXX_VISIBILITY_PRESET hidden)
else()
  target_compile_definitions(nobion_scripting PUBLIC NOBION_SCRIPTING_ENABLED=0)
endif()

nobion_set_warnings(nobion_scripting)
