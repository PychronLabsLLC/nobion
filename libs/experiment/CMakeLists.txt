# nobion::experiment. Sources are globbed so units landing in parallel only add
# files under src/ and include/nobion/experiment/ and never edit this list.
file(GLOB_RECURSE NOBION_EXPERIMENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# src/metrics/ reports runs and queues to the Prometheus registry; without
# libs/metrics (NOBION_METRICS=OFF) it is left out.
if(NOT TARGET nobion_metrics)
  list(FILTER NOBION_EXPERIMENT_SOURCES EXCLUDE REGEX "/src/metrics/")
endif()

add_library(nobion_experiment STATIC ${NOBION_EXPERIMENT_SOURCES})
add_library(nobion::experiment ALIAS nobion_experiment)

target_include_directories(nobion_experiment PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
# Headers shared between units of the library but not part of its interface
# (model/toml_text.hpp), included as "model/<name>.hpp".
target_include_directories(nobion_experiment PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(nobion_experiment PUBLIC cxx_std_20)
target_link_libraries(nobion_experiment PUBLIC nobion::systems nobion::scripting nobion::reduction nobion::laser nobion::devices nobion::core)
# nlohmann_json reads the executor's state file back (Executor::read_state) and writes
# the resolved-plan golden form (src/plan/plan_json.cpp); not in public headers.
target_link_libraries(nobion_experiment PRIVATE nlohmann_json::nlohmann_json)

if(TARGET nobion_metrics)
  target_link_libraries(nobion_experiment PUBLIC nobion::metrics)
  target_compile_definitions(nobion_experiment PUBLIC NOBION_EXPERIMENT_HAS_METRICS=1)
endif()

nobion_set_warnings(nobion_experiment)

# Turns a finished record into what the DVC store ingests. Its own library so
# that nobion::experiment never depends on persistence; it exists only when
# persistence is built.
if(TARGET nobion_persistence)
  file(GLOB NOBION_EXPERIMENT_STORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/src/*.cpp")
  add_library(nobion_experiment_store STATIC ${NOBION_EXPERIMENT_STORE_SOURCES})
  add_library(nobion::experiment_store ALIAS nobion_experiment_store)
  target_include_directories(nobion_experiment_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/store/include>
    $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(nobion_experiment_store PUBLIC nobion::experiment nobion::persistence
    PRIVATE nlohmann_json::nlohmann_json)
  target_compile_definitions(nobion_experiment_store PUBLIC NOBION_EXPERIMENT_HAS_STORE=1)
  nobion_set_warnings(nobion_experiment_store)
endif()
