# Some macOS Command Line Tools installs leave a stale, partial
# <CLT>/usr/include/c++/v1 directory that shadows the SDK's complete libc++
# headers (symptom: "'chrono' file not found"). Detect that and point the
# compiler at the SDK's libc++ headers instead.
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  include(CheckIncludeFileCXX)
  check_include_file_cxx(version NOBION_HAVE_STD_VERSION_HEADER)
  if(NOT NOBION_HAVE_STD_VERSION_HEADER)
    set(_nobion_sdk "${CMAKE_OSX_SYSROOT}")
    if(NOT _nobion_sdk)
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE _nobion_sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    set(_nobion_sdk_libcxx "${_nobion_sdk}/usr/include/c++/v1")
    if(_nobion_sdk AND EXISTS "${_nobion_sdk_libcxx}/version")
      message(STATUS "nobion: toolchain libc++ headers incomplete; using ${_nobion_sdk_libcxx}")
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
                          "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-isystem ${_nobion_sdk_libcxx}>")
    else()
      message(WARNING "nobion: <version> header not found and no SDK libc++ fallback found")
    endif()
  endif()
endif()

# MSVC's RelWithDebInfo links /INCREMENTAL and keeps unreferenced code and
# duplicate functions (/OPT:REF and /OPT:ICF are off with /DEBUG): not what a
# release is. Put back what Release does, and keep the PDB complete
# (/DEBUG:FULL), which release.yml's symbols archive needs.
if(MSVC)
  foreach(_kind EXE SHARED MODULE)
    set(CMAKE_${_kind}_LINKER_FLAGS_RELWITHDEBINFO "/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF")
  endforeach()
endif()
