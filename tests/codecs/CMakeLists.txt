# One test_<vendor>.cpp per vendor codec (see libs/codecs/CONVENTIONS.md);
# spectrometer codecs live under spectrometer/.
file(GLOB NOBION_CODEC_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/spectrometer/test_*.cpp")

add_executable(nobion_codecs_tests ${NOBION_CODEC_TESTS})

target_link_libraries(nobion_codecs_tests PRIVATE nobion::codecs nlohmann_json::nlohmann_json GTest::gtest_main)
nobion_set_warnings(nobion_codecs_tests)

gtest_discover_tests(nobion_codecs_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)

target_compile_definitions(nobion_codecs_tests PRIVATE
  NOBION_TRACES_DIR="${PROJECT_SOURCE_DIR}/tests/traces"
  NOBION_QTEGRA_CODEC_SOURCE="${PROJECT_SOURCE_DIR}/libs/codecs/src/thermo_qtegra.cpp")
