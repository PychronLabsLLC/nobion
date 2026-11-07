# nobion::dvc — the adapter for legacy Python-pychron DVC repositories
# (legacy ingestion spec, sections 4 and 10): the pure layer (path to file
# kind, legacy JSON to store payloads), GitReader, which reads a repository's
# history through git subprocesses, the project and meta repository
# adapters built on both, and the catalog adapter, which reads a converted
# dump of the legacy database. Public headers are std-only;
# nlohmann_json stays behind the library boundary.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

add_library(nobion_dvc STATIC
  src/catalog_adapter.cpp
  src/commit_walk.cpp
  src/git_reader.cpp
  src/legacy_interpreted_age.cpp
  src/legacy_json.cpp
  src/legacy_path.cpp
  src/legacy_record.cpp
  src/legacy_revision.cpp
  src/meta_adapter.cpp
  src/meta_files.cpp
  src/meta_layout.cpp
  src/project_adapter.cpp
  src/project_map.cpp
  src/project_units.cpp
  src/project_walk.cpp
)
add_library(nobion::dvc ALIAS nobion_dvc)

target_include_directories(nobion_dvc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nobion_dvc PUBLIC cxx_std_20)
target_link_libraries(nobion_dvc
  PUBLIC nobion::ingest
  PRIVATE nlohmann_json::nlohmann_json)

nobion_set_warnings(nobion_dvc)
