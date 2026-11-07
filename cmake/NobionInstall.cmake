# Install layout and packages (installation wizard spec section 3.7).
#
#   Linux, Windows             macOS (with BUILD_UI)
#   <prefix>/bin/elctl         Nobion.app/Contents/MacOS/Nobion   (nobion)
#   <prefix>/bin/nobion        Nobion.app/Contents/MacOS/elctl
#   <prefix>/share/nobion/    Nobion.app/Contents/Resources/
#     profiles/ examples/        profiles/ examples/
#     earthbank/templates/       earthbank/templates/
#     python/  (optional)        python/  (optional)
#
# setup::find_resources(), earthbank::default_template_dir() and the scripting
# host look for these next to the running program, so an install is
# relocatable. Everything nobion installs
# is in the "nobion" component (the Qt runtime copy in "nobion_qt"); the
# bundled dependencies' own install rules (headers, static libraries) are left
# out of packages.
#
#   cmake --install <build> --component nobion --prefix <dir>
#   cpack --config <build>/CPackConfig.cmake          (DEB+TGZ, DragNDrop, NSIS+ZIP)
#
# NOBION_BUNDLE_PYTHON: a relocatable CPython (python-build-standalone
# "install_only") to ship as python/, for scripted extractions. Configure with
# Python_ROOT_DIR pointing at the same directory so the programs link its
# libpython; they find it through their RPATH (Linux, macOS) or beside them
# (Windows). Without it the programs use the Python they were built against.
#
# NOBION_DEPLOY_QT copies the Qt libraries and plugins in (macdeployqt,
# windeployqt; Qt >= 6.5 on Linux). On by default on macOS and Windows; off on
# Linux, where the .deb depends on the distribution's Qt.

include(GNUInstallDirs)

set(NOBION_BUNDLE_PYTHON "" CACHE PATH "Relocatable CPython (python-build-standalone install_only) to ship with the programs")
if(APPLE OR WIN32)
  set(_nobion_deploy_qt_default ON)
else()
  set(_nobion_deploy_qt_default OFF)
endif()
option(NOBION_DEPLOY_QT "Copy the Qt runtime into the install (macdeployqt / windeployqt)" ${_nobion_deploy_qt_default})

set(NOBION_COMPONENT nobion)
if(APPLE AND TARGET nobion)
  set(NOBION_INSTALL_BUNDLE "Nobion.app")
  set(NOBION_INSTALL_BIN "${NOBION_INSTALL_BUNDLE}/Contents/MacOS")
  set(NOBION_INSTALL_RESOURCES "${NOBION_INSTALL_BUNDLE}/Contents/Resources")
  set(NOBION_INSTALL_DOC "${NOBION_INSTALL_RESOURCES}")
  set(_nobion_python_rpath "@executable_path/../Resources/python/lib")
else()
  set(NOBION_INSTALL_BIN "${CMAKE_INSTALL_BINDIR}")
  set(NOBION_INSTALL_RESOURCES "${CMAKE_INSTALL_DATADIR}/nobion")
  set(NOBION_INSTALL_DOC "${CMAKE_INSTALL_DATADIR}/doc/nobion")
  if(APPLE)
    set(_nobion_python_rpath "@executable_path/../share/nobion/python/lib")
  else()
    set(_nobion_python_rpath "$ORIGIN/../share/nobion/python/lib")
  endif()
endif()

# --- programs -----------------------------------------------------------------

set(_nobion_programs)
if(TARGET elctl)
  list(APPEND _nobion_programs elctl)
  install(TARGETS elctl RUNTIME DESTINATION "${NOBION_INSTALL_BIN}" COMPONENT ${NOBION_COMPONENT})
endif()
if(TARGET nobion)
  list(APPEND _nobion_programs nobion)
  if(APPLE)
    set_target_properties(nobion PROPERTIES
      MACOSX_BUNDLE ON
      OUTPUT_NAME Nobion
      MACOSX_BUNDLE_BUNDLE_NAME Nobion
      MACOSX_BUNDLE_GUI_IDENTIFIER com.pychronlabs.nobion
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_ICON_FILE nobion.icns
      # CMake's own template plus NSCameraUsageDescription: without it macOS
      # ends a program that opens a camera (a laser's live camera).
      MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in")
    # The icon files are the application icon rendered by tools/make_icons.py.
    set(_nobion_icns "${PROJECT_SOURCE_DIR}/packaging/icons/nobion.icns")
    # TARGET_DIRECTORY: source properties belong to a directory, and the target
    # is made in apps/nobion, not here; without it the icon is not copied.
    set_source_files_properties("${_nobion_icns}" TARGET_DIRECTORY nobion
      PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(nobion PRIVATE "${_nobion_icns}")
  elseif(WIN32)
    set_target_properties(nobion PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(nobion PRIVATE "${PROJECT_SOURCE_DIR}/packaging/windows/nobion.rc")
  endif()
  install(TARGETS nobion
    BUNDLE DESTINATION . COMPONENT ${NOBION_COMPONENT}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${NOBION_COMPONENT})
endif()

# --- shared files ---------------------------------------------------------------

install(DIRECTORY "${PROJECT_SOURCE_DIR}/profiles/" DESTINATION "${NOBION_INSTALL_RESOURCES}/profiles"
  COMPONENT ${NOBION_COMPONENT})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/configs/examples/" DESTINATION "${NOBION_INSTALL_RESOURCES}/examples"
  COMPONENT ${NOBION_COMPONENT})
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${NOBION_INSTALL_DOC}"
  COMPONENT ${NOBION_COMPONENT})
# EarthBank's workbook templates, the vendor's files as they are (not the note
# kept beside them in the source tree).
if(TARGET nobion_earthbank)
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/libs/earthbank/templates/"
    DESTINATION "${NOBION_INSTALL_RESOURCES}/earthbank/templates"
    COMPONENT ${NOBION_COMPONENT} FILES_MATCHING PATTERN "*.xlsx")
endif()

# The licence texts a release directory carries (libs/publish, LICENSE.txt).
if(TARGET nobion_publish)
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/libs/publish/licences/"
    DESTINATION "${NOBION_INSTALL_RESOURCES}/publish/licences"
    COMPONENT ${NOBION_COMPONENT} FILES_MATCHING PATTERN "*.txt")
endif()

if(UNIX AND NOT APPLE AND TARGET nobion)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/nobion.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT ${NOBION_COMPONENT})
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/nobion.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps" COMPONENT ${NOBION_COMPONENT})
endif()

# --- bundled Python -------------------------------------------------------------

if(NOBION_BUNDLE_PYTHON)
  if(NOT EXISTS "${NOBION_BUNDLE_PYTHON}")
    message(FATAL_ERROR "NOBION_BUNDLE_PYTHON: ${NOBION_BUNDLE_PYTHON} does not exist")
  endif()
  # The embedded interpreter needs libpython and the standard library only:
  # no python executable, headers, Tcl/Tk, pip or the developer tools.
  install(DIRECTORY "${NOBION_BUNDLE_PYTHON}/" DESTINATION "${NOBION_INSTALL_RESOURCES}/python"
    USE_SOURCE_PERMISSIONS COMPONENT ${NOBION_COMPONENT}
    REGEX "/(bin|include|share|Scripts|tcl|libs)$" EXCLUDE
    REGEX "/lib/(tcl|tk|itcl|thread|pkgconfig)[^/]*$" EXCLUDE
    REGEX "/lib/libt(cl|k)[^/]*$" EXCLUDE
    REGEX "/lib/libpython3\\.(so|dylib)$" EXCLUDE  # the stable-ABI stub: unused, and its $ORIGIN link breaks dpkg-shlibdeps
    REGEX "/(__pycache__|test|tests|idlelib|tkinter|turtledemo|ensurepip|lib2to3|pydoc_data|venv)$" EXCLUDE
    REGEX "/site-packages/[^/]+$" EXCLUDE
    REGEX "/lib-dynload/_tkinter[^/]*$" EXCLUDE
    REGEX "/config-[^/]*$" EXCLUDE
    REGEX "\\.(exe|pdb|lib)$" EXCLUDE)
  if(NOT WIN32 AND CMAKE_STRIP)
    # python-build-standalone ships its libraries with debug info (~200 MB);
    # strip it from the installed copies (-S: debug symbols only).
    install(CODE "
      file(GLOB_RECURSE _libs
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${NOBION_INSTALL_RESOURCES}/python/lib/*.so*\"
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${NOBION_INSTALL_RESOURCES}/python/lib/*.dylib\")
      foreach(_lib IN LISTS _libs)
        if(NOT IS_SYMLINK \"\${_lib}\")
          execute_process(COMMAND \"${CMAKE_STRIP}\" -S \"\${_lib}\")
          if(APPLE)  # a changed library needs signing again (ad hoc) to load
            execute_process(COMMAND codesign --force --sign - \"\${_lib}\")
          endif()
        endif()
      endforeach()" COMPONENT ${NOBION_COMPONENT})
  endif()
  if(WIN32)
    # The DLL loader looks beside the program.
    file(GLOB _nobion_python_dlls "${NOBION_BUNDLE_PYTHON}/python3*.dll")
    install(FILES ${_nobion_python_dlls} DESTINATION "${NOBION_INSTALL_BIN}" COMPONENT ${NOBION_COMPONENT})
  else()
    foreach(program IN LISTS _nobion_programs)
      set_property(TARGET ${program} APPEND PROPERTY INSTALL_RPATH "${_nobion_python_rpath}")
    endforeach()
  endif()
endif()

# --- Qt runtime -----------------------------------------------------------------

if(NOBION_DEPLOY_QT AND TARGET nobion)
  if(UNIX AND NOT APPLE AND Qt6_VERSION VERSION_LESS 6.5)
    message(WARNING "NOBION_DEPLOY_QT: Qt ${Qt6_VERSION} cannot deploy on Linux (needs 6.5); skipped")
  elseif(COMMAND qt6_generate_deploy_script)
    # nobion and elctl share one copy of Qt (elctl uses QtSql for the store).
    if(APPLE)
      set(_nobion_ui_exe "\${QT_DEPLOY_PREFIX}/${NOBION_INSTALL_BIN}/Nobion")
    else()
      set(_nobion_ui_exe "\${QT_DEPLOY_PREFIX}/${NOBION_INSTALL_BIN}/$<TARGET_FILE_NAME:nobion>")
    endif()
    set(_nobion_extra "")
    if(TARGET elctl)
      set(_nobion_extra "ADDITIONAL_EXECUTABLES \"\${QT_DEPLOY_PREFIX}/${NOBION_INSTALL_BIN}/$<TARGET_FILE_NAME:elctl>\"")
    endif()
    # qt6_, not qt_: the versionless name is a macro that forwards ${ARGV}, so
    # CMake expands ${QT_DEPLOY_PREFIX} at configure time (to nothing) and the
    # deploy tool is handed "/Nobion.app".
    qt6_generate_deploy_script(
      TARGET nobion
      OUTPUT_SCRIPT _nobion_deploy_script
      CONTENT "
qt_deploy_runtime_dependencies(
  EXECUTABLE \"${_nobion_ui_exe}\"
  ${_nobion_extra}
  GENERATE_QT_CONF
)")
    # Its own component: the install test (tests/setup) skips it.
    install(SCRIPT "${_nobion_deploy_script}" COMPONENT nobion_qt)
  else()
    message(WARNING "NOBION_DEPLOY_QT: this Qt has no qt6_generate_deploy_script (Qt >= 6.5); skipped")
  endif()
endif()

include(NobionPackaging)
