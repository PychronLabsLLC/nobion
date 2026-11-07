# nobion::processing (data browsing and visualization design): analysis model,
# quantities, datasets, options and presets, composable units, scenes. Qt-free;
# depends on core and reduction only. Sources are globbed so parallel units
# only add files.
file(GLOB NOBION_PROCESSING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_processing STATIC ${NOBION_PROCESSING_SOURCES})
add_library(nobion::processing ALIAS nobion_processing)

target_include_directories(nobion_processing PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(nobion_processing PUBLIC cxx_std_20)
target_link_libraries(nobion_processing PUBLIC nobion::reduction nobion::core)
nobion_set_warnings(nobion_processing)

# The AnalysisRecord directory source links the experiment library, so it is a
# separate target: nobion::processing never pulls in the experiment stack.
if(TARGET nobion_experiment)
  file(GLOB NOBION_PROCESSING_RECORDS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/src/*.cpp")
  add_library(nobion_processing_records STATIC ${NOBION_PROCESSING_RECORDS_SOURCES})
  add_library(nobion::processing_records ALIAS nobion_processing_records)
  target_include_directories(nobion_processing_records PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(nobion_processing_records PUBLIC nobion::processing nobion::experiment)
  nobion_set_warnings(nobion_processing_records)
endif()

# The DVC store source links the persistence library (TinyORM on QtSql behind
# a Qt-free interface); it exists only when persistence is built.
if(TARGET nobion_persistence)
  file(GLOB NOBION_PROCESSING_STORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/src/*.cpp")
  add_library(nobion_processing_store STATIC ${NOBION_PROCESSING_STORE_SOURCES})
  add_library(nobion::processing_store ALIAS nobion_processing_store)
  target_include_directories(nobion_processing_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(nobion_processing_store PUBLIC nobion::processing nobion::persistence
    PRIVATE nlohmann_json::nlohmann_json)
  nobion_set_warnings(nobion_processing_store)
endif()
