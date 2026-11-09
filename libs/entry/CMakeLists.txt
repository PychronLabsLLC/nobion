# nobion::entry — sample and package entry (2026-10-04 entry spec, section
# 6): validation, CSV import, the level-sheet edit model and the identifier
# planner over the DVC store. Qt-free; public headers are std-only.
if(NOT NOBION_PERSISTENCE_ENABLED)
  return()
endif()

add_library(nobion_entry STATIC
  src/csv.cpp
  src/export.cpp
  src/holder_import.cpp
  src/identifier_plan.cpp
  src/level_sheet.cpp
  src/names.cpp
  src/package_edit.cpp
  src/positions_import.cpp
  src/sample_fields.cpp
  src/sample_import.cpp
  src/sample_search.cpp
  src/seed.cpp
  src/settings.cpp
)
add_library(nobion::entry ALIAS nobion_entry)

target_include_directories(nobion_entry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nobion_entry PUBLIC cxx_std_20)
# dvc for the legacy holder and chronology formats; nlohmann_json for the
# settings and reactors documents. Neither appears in a public header.
target_link_libraries(nobion_entry
  PUBLIC nobion::persistence nobion::core
  PRIVATE nobion::dvc nlohmann_json::nlohmann_json)

nobion_set_warnings(nobion_entry)
