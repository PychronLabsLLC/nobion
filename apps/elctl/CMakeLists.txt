# elctl: headless CLI for bring-up and scripting (spec section 9.3).
#
# Everything but main() lives in nobion_elctl_lib so the tests under
# tests/ drive the exact command code the binary runs.
file(GLOB NOBION_ELCTL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM NOBION_ELCTL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# `elctl import` (legacy ingestion spec, section 5) needs the DVC store. With
# persistence its sources are src/import*.cpp; without, only import_stub.cpp,
# which says so and exits 2. Never both.
# `elctl entry` (entry spec, section 10), `elctl export` (the Schaen et
# al. 2021 report, read from the store), `elctl flux` and `elctl db` (the
# store's schema) are split the same way.
# So is `elctl earthbank` (the EarthBank workbooks, filled from the store).
# src/selection.cpp (the analyses `export` and `earthbank` select, read from
# the store) has no stub: nothing uses it without persistence.
set(NOBION_ELCTL_IMPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/import_stub.cpp")
set(NOBION_ELCTL_ENTRY_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/entry_stub.cpp")
set(NOBION_ELCTL_EXPORT_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/export_stub.cpp")
set(NOBION_ELCTL_EARTHBANK_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/earthbank_stub.cpp")
set(NOBION_ELCTL_FLUX_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/flux_stub.cpp")
set(NOBION_ELCTL_DB_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/db_stub.cpp")
# `elctl publish` and `elctl embargo` (data publishing spec 6.2) are split the same way.
set(NOBION_ELCTL_PUBLISH_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/publish_stub.cpp")
set(NOBION_ELCTL_EMBARGO_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/embargo_stub.cpp")
# `exp run`'s database (src/exp_store.cpp) is split the same way.
set(NOBION_ELCTL_EXP_STORE_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/exp_store_stub.cpp")
# `elctl earthbank login|account|packages|preflight` talk to the service through
# libs/earthbank_qt (Qt6 Network). Without it src/earthbank_no_network.cpp
# gives make_http_client() as null and those commands say so and exit 2.
set(NOBION_ELCTL_NO_NETWORK "${CMAKE_CURRENT_SOURCE_DIR}/src/earthbank_no_network.cpp")
list(REMOVE_ITEM NOBION_ELCTL_SOURCES "${NOBION_ELCTL_NO_NETWORK}")
if(TARGET nobion_persistence)
  list(REMOVE_ITEM NOBION_ELCTL_SOURCES "${NOBION_ELCTL_IMPORT_STUB}" "${NOBION_ELCTL_ENTRY_STUB}"
    "${NOBION_ELCTL_EXPORT_STUB}" "${NOBION_ELCTL_EARTHBANK_STUB}" "${NOBION_ELCTL_FLUX_STUB}"
    "${NOBION_ELCTL_DB_STUB}" "${NOBION_ELCTL_EXP_STORE_STUB}" "${NOBION_ELCTL_PUBLISH_STUB}"
    "${NOBION_ELCTL_EMBARGO_STUB}")
  if(NOT TARGET nobion_earthbank_qt)
    list(APPEND NOBION_ELCTL_SOURCES "${NOBION_ELCTL_NO_NETWORK}")
  endif()
else()
  list(FILTER NOBION_ELCTL_SOURCES EXCLUDE REGEX "/src/(import|entry|export|earthbank|flux|db|exp_store|selection|publish|embargo)[^/]*\\.cpp$")
  list(APPEND NOBION_ELCTL_SOURCES "${NOBION_ELCTL_IMPORT_STUB}" "${NOBION_ELCTL_ENTRY_STUB}"
    "${NOBION_ELCTL_EXPORT_STUB}" "${NOBION_ELCTL_EARTHBANK_STUB}" "${NOBION_ELCTL_FLUX_STUB}"
    "${NOBION_ELCTL_DB_STUB}" "${NOBION_ELCTL_EXP_STORE_STUB}" "${NOBION_ELCTL_PUBLISH_STUB}"
    "${NOBION_ELCTL_EMBARGO_STUB}")
endif()

add_library(nobion_elctl_lib STATIC ${NOBION_ELCTL_SOURCES})
target_include_directories(nobion_elctl_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(nobion_elctl_lib PUBLIC cxx_std_20)
# processing and arar: the domains every command runs on (src/domain.cpp), in
# every build.
target_link_libraries(nobion_elctl_lib PUBLIC nobion::setup nobion::experiment nobion::systems nobion::devices
  nobion::processing nobion::arar nobion::transport nobion::update nobion::core)
if(TARGET nobion_persistence)
  # A data reduction install's local database is created with the DVC store.
  # processing_store: the reduction `import verify` compares legacy ages with,
  # and the source `export` and `earthbank` read. arar_store: the flux store
  # `flux` reads and writes. earthbank: the submissions and the workbooks; it
  # has no network code. Qt6::Core (persistence's own Qt): QUrl, so that
  # `db migrate` prints a store URL without its password as the store parses it.
  target_link_libraries(nobion_elctl_lib PUBLIC nobion_persistence nobion::dvc nobion::ingest nobion::entry
    nobion::processing_store nobion::arar_store nobion::experiment_store nobion::earthbank nobion::publish
    PRIVATE nlohmann_json::nlohmann_json Qt6::Core)
  target_compile_definitions(nobion_elctl_lib PUBLIC NOBION_ELCTL_HAS_STORE=1 NOBION_ELCTL_IMPORT
    PRIVATE QT_NO_KEYWORDS)  # the store's headers name members `signals`
endif()
if(TARGET nobion_earthbank_qt)
  target_link_libraries(nobion_elctl_lib PUBLIC nobion::earthbank_qt)
  target_compile_definitions(nobion_elctl_lib PUBLIC NOBION_ELCTL_HAS_NETWORK=1)
endif()
nobion_set_warnings(nobion_elctl_lib)

add_executable(elctl src/main.cpp)
target_link_libraries(elctl PRIVATE nobion_elctl_lib)
# sim spectrometer drivers register from static initializers nothing references
nobion_link_drivers(elctl nobion_sim)
nobion_set_warnings(elctl)

# App tests live beside the app: the top-level tests/ loop only covers libs/.
if(BUILD_TESTS)
  enable_testing()
  include(GoogleTest)
  add_subdirectory(tests)
endif()
