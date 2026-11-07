# Per-vendor byte codecs: pure functions, zero I/O. Each vendor adds
# include/nobion/codecs/<vendor>.hpp and src/<vendor>.cpp (CONVENTIONS.md).
file(GLOB NOBION_CODEC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_codecs STATIC ${NOBION_CODEC_SOURCES})
add_library(nobion::codecs ALIAS nobion_codecs)

target_include_directories(nobion_codecs PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_codecs PUBLIC cxx_std_20)
# Codecs need Bytes/ReadSpec vocabulary only; they never open a transport.
target_link_libraries(nobion_codecs PUBLIC nobion::transport)
# Qtegra's Describe reply is JSON; it stays behind the codec's interface.
target_link_libraries(nobion_codecs PRIVATE nlohmann_json::nlohmann_json)

nobion_set_warnings(nobion_codecs)
