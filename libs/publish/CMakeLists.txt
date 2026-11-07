# nobion::publish: a release of a lab's reduced data (data publishing and
# hosted sharing design): the row tables of a release, its manifest and OGC
# API - Records catalog, the build of the release directory, and the release,
# embargo and registration workflows over the store. Qt-free. Built only with
# the store (persistence) it reads. Apache Arrow (Parquet) is optional and is
# seen by src/parquet_writer.cpp alone; without it make_parquet_writer() is
# null and a build writes everything but data/*.parquet.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

file(GLOB NOBION_PUBLISH_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_publish STATIC ${NOBION_PUBLISH_SOURCES})
add_library(nobion::publish ALIAS nobion_publish)

target_include_directories(nobion_publish PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(nobion_publish PUBLIC cxx_std_20)
target_link_libraries(nobion_publish PUBLIC nobion::persistence nobion::arar nobion::processing nobion::processing_store
                                            nobion::reduction nobion::core nlohmann_json::nlohmann_json)
nobion_set_warnings(nobion_publish)
# Arrow (NobionDependencies.cmake): parquet_writer.cpp alone sees it; without
# it that file is empty and parquet_stub.cpp gives a null writer.
if(NOBION_PUBLISH_ARROW_ENABLED)
  target_link_libraries(nobion_publish PRIVATE ${NOBION_PUBLISH_ARROW_LIBS})
  target_compile_definitions(nobion_publish PRIVATE NOBION_PUBLISH_ARROW_ENABLED)
endif()
# The licence texts a release ships (LICENSE.txt): installed beside the
# program (cmake/NobionInstall.cmake); an uninstalled build finds them here.
target_compile_definitions(nobion_publish PRIVATE
  NOBION_PUBLISH_SOURCE_LICENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/licences")
