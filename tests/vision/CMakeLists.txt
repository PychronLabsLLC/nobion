# One test_<component>.cpp per vision component. Globbed so parallel units only add files.
if(NOT NOBION_VISION)
  return()
endif()

file(GLOB NOBION_VISION_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

if(NOBION_VISION_TESTS)
  add_executable(nobion_vision_tests ${NOBION_VISION_TESTS})
  target_link_libraries(nobion_vision_tests PRIVATE nobion::vision GTest::gtest_main)
  target_compile_definitions(nobion_vision_tests PRIVATE
    NOBION_VISION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  # Only the OpenCvSource tests use OpenCV (to write a test video).
  if(NOBION_VISION_OPENCV_ENABLED)
    target_include_directories(nobion_vision_tests SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(nobion_vision_tests PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(nobion_vision_tests PRIVATE NOBION_VISION_OPENCV_ENABLED)
  endif()
  nobion_set_warnings(nobion_vision_tests)
  gtest_discover_tests(nobion_vision_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
endif()
