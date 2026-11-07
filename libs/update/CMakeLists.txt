# nobion::update: telling the user that a newer release exists. Reads
# versions and GitHub's "latest release" answer. Qt-free and without network
# code: what it asks goes through nobion::http::IHttpClient (libs/core),
# which a program gives it. Sources are globbed so units landing in parallel
# only add files.
file(GLOB NOBION_UPDATE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_update STATIC ${NOBION_UPDATE_SOURCES})
add_library(nobion::update ALIAS nobion_update)

target_include_directories(nobion_update PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(nobion_update PUBLIC cxx_std_20)
target_link_libraries(nobion_update PUBLIC nobion::core PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(nobion_update PRIVATE NOBION_UPDATE_USER_AGENT="nobion/${PROJECT_VERSION}")
nobion_set_warnings(nobion_update)
