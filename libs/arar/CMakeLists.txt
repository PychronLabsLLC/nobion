# libs/arar: the Ar-Ar domain (domain seam spec 3.1).
#
# nobion::arar_reduction: the Ar-Ar reduction (types, reduce(), decay, flux
# math, the live-parity bridge). Links nobion::reduction only. Sources are
# globbed so units landing in parallel only add files under src/reduction/.
file(GLOB_RECURSE NOBION_ARAR_REDUCTION_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/reduction/*.cpp")

add_library(nobion_arar_reduction STATIC ${NOBION_ARAR_REDUCTION_SOURCES})
add_library(nobion::arar_reduction ALIAS nobion_arar_reduction)

target_include_directories(nobion_arar_reduction PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(nobion_arar_reduction PUBLIC nobion::reduction)
nobion_set_warnings(nobion_arar_reduction)

# Parity with Python's per-operation rounding (reduction spec 4.7): never fuse
# a * b + c into an FMA. nobion::reduction already sets it PUBLIC; repeated
# here so this library's own arithmetic does not depend on that. MSVC's
# default /fp:precise does not contract across statements and needs nothing.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nobion_arar_reduction PUBLIC -ffp-contract=off)
endif()

# nobion::arar: figures (ideogram, spectrum, inverse isochron, spectrum +
# isochron), the Schaen et al. (2021) report, group ages, the derived age, the
# flux fit and view, and the Ar-Ar Domain. Links processing; processing never
# links it. Sources are globbed (top level of src/ only: src/reduction/ is
# nobion::arar_reduction's).
file(GLOB NOBION_ARAR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_arar STATIC ${NOBION_ARAR_SOURCES})
add_library(nobion::arar ALIAS nobion_arar)

target_include_directories(nobion_arar
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  # The figures share processing's private figure helpers (figure_common.hpp,
  # schema_builder.hpp) with the neutral figures that stay there.
  PRIVATE ${PROJECT_SOURCE_DIR}/libs/processing/src
)
target_link_libraries(nobion_arar PUBLIC nobion::processing nobion::arar_reduction)
nobion_set_warnings(nobion_arar)

# nobion::arar_store: the flux store (monitor sets, a level's saved fit, flux
# options) and the store source's Ar-Ar extension loader over the DVC store;
# exists only when processing's store adapter does.
if(TARGET nobion_processing_store)
  file(GLOB NOBION_ARAR_STORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/src/*.cpp")
  add_library(nobion_arar_store STATIC ${NOBION_ARAR_STORE_SOURCES})
  add_library(nobion::arar_store ALIAS nobion_arar_store)
  target_include_directories(nobion_arar_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(nobion_arar_store PUBLIC nobion::arar nobion::processing_store
    PRIVATE nlohmann_json::nlohmann_json)
  nobion_set_warnings(nobion_arar_store)
endif()

# nobion::arar_records: the Ar-Ar extension loader of a records directory
# (references.toml); exists only when processing's records adapter does.
if(TARGET nobion_processing_records)
  file(GLOB NOBION_ARAR_RECORDS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/src/*.cpp")
  add_library(nobion_arar_records STATIC ${NOBION_ARAR_RECORDS_SOURCES})
  add_library(nobion::arar_records ALIAS nobion_arar_records)
  target_include_directories(nobion_arar_records PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/records/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(nobion_arar_records PUBLIC nobion::arar nobion::processing_records)
  nobion_set_warnings(nobion_arar_records)
endif()
