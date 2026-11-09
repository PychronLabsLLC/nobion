# One test_<component>.cpp per earthbank component. Globbed so parallel units
# only add files. The mapping's fixtures are built on the Ar-Ar tests'
# (tests/arar/arar_fixtures.hpp), included as "arar/arar_fixtures.hpp".
# test_ooxml.cpp tests an internal unit, and test_xlsx.cpp reads workbooks back
# with it: they include "ooxml.hpp" from the library's src/ and read the
# vendor templates where the library keeps them. test_secret_store.cpp includes
# "secret_store_detail.hpp" from there too. test_profiles.cpp reads the tree
# (NOBION_EARTHBANK_REPO_DIR) to see that the service's host is named in one
# file only.
file(GLOB NOBION_EARTHBANK_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# nobion::earthbank_fake: EarthBank in memory (fake_earthbank.hpp), the only
# one a test of the REST route talks to: the real service is in production and
# has no sandbox. A library of its own so that the tests of the programs
# (apps/elctl/tests, the UI's) link the same one; it brings its include
# directory and where its fixtures are. Nothing under libs/ or apps/*/src may
# depend on it.
add_library(nobion_earthbank_fake STATIC fake_earthbank.cpp)
add_library(nobion::earthbank_fake ALIAS nobion_earthbank_fake)
target_include_directories(nobion_earthbank_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nobion_earthbank_fake PUBLIC
  NOBION_EARTHBANK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(nobion_earthbank_fake PUBLIC nobion::earthbank nobion::core nlohmann_json::nlohmann_json)
nobion_set_warnings(nobion_earthbank_fake)

add_executable(nobion_earthbank_tests ${NOBION_EARTHBANK_TESTS})
target_include_directories(nobion_earthbank_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(nobion_earthbank_tests PRIVATE ${PROJECT_SOURCE_DIR}/libs/earthbank/src)
target_compile_definitions(nobion_earthbank_tests PRIVATE
  NOBION_EARTHBANK_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/libs/earthbank/templates"
  NOBION_EARTHBANK_REPO_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(nobion_earthbank_tests PRIVATE nobion::earthbank_fake nobion::earthbank nobion::arar nobion::processing
                                                      nobion::reduction GTest::gtest_main)
nobion_set_warnings(nobion_earthbank_tests)
gtest_discover_tests(nobion_earthbank_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
