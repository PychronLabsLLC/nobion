# nobion::earthbank_qt: EarthBank over HTTPS, on Qt6 Network. The only code
# of ours that opens a socket to the service; libs/earthbank says what to ask
# and goes through IHttpClient. Qt is PRIVATE: the public header is std-only.
# Built only when Qt6 Core and Network are found (cmake/NobionDependencies.cmake).
if(NOT NOBION_EARTHBANK_NETWORK_ENABLED)
  return()
endif()

add_library(nobion_earthbank_qt STATIC src/qt_http_client.cpp)
add_library(nobion::earthbank_qt ALIAS nobion_earthbank_qt)

target_include_directories(nobion_earthbank_qt PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(nobion_earthbank_qt PUBLIC cxx_std_20)
target_link_libraries(nobion_earthbank_qt PUBLIC nobion::earthbank nobion::core
                                           PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(nobion_earthbank_qt PRIVATE QT_NO_KEYWORDS
  NOBION_EARTHBANK_USER_AGENT="nobion/${PROJECT_VERSION}")
nobion_set_warnings(nobion_earthbank_qt)
