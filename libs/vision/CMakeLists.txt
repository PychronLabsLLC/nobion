# nobion::vision: frames, image kernel, target finding. Depends on core only.
if(NOT NOBION_VISION)
  return()
endif()

file(GLOB_RECURSE NOBION_VISION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_vision STATIC ${NOBION_VISION_SOURCES})
add_library(nobion::vision ALIAS nobion_vision)

target_include_directories(nobion_vision PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_vision PUBLIC cxx_std_20)
target_link_libraries(nobion_vision PUBLIC nobion::core)

# legacy_finder.cpp and opencv_source.cpp are always compiled; each reduces to
# its stub without NOBION_VISION_OPENCV_ENABLED. SYSTEM keeps OpenCV's own
# warnings away from -Werror.
if(NOBION_VISION_OPENCV_ENABLED)
  target_include_directories(nobion_vision SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(nobion_vision PRIVATE ${OpenCV_LIBS})
  target_compile_definitions(nobion_vision PRIVATE NOBION_VISION_OPENCV_ENABLED)
endif()

nobion_set_warnings(nobion_vision)
