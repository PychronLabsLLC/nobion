# Qt Test suites of apps/grains, run headless on the offscreen platform. Two
# QTEST_MAIN cannot share a binary, so test_main.cpp runs each class.
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(pychron_grains_app_tests
  test_main.cpp
  test_grain_overlay.cpp
  test_grain_overlay.hpp
  test_grains_window.cpp
  test_grains_window.hpp
  test_replay_command.cpp
  test_replay_command.hpp
)
target_link_libraries(pychron_grains_app_tests PRIVATE pychron_grains_app_lib Qt6::Test)
pychron_set_warnings(pychron_grains_app_tests)

add_test(NAME grains_app.qt COMMAND pychron_grains_app_tests)
set_tests_properties(grains_app.qt PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  TIMEOUT 300)
