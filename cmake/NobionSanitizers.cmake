# Applies NOBION_SANITIZE (e.g. "address,undefined") to every target in the
# build, including third-party ones, so instrumented and uninstrumented code
# never mix.
if(NOBION_SANITIZE)
  if(MSVC)
    if(NOBION_SANITIZE MATCHES "address")
      add_compile_options(/fsanitize=address)
    endif()
  else()
    add_compile_options(-fsanitize=${NOBION_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${NOBION_SANITIZE})
    # The standard library's own checks, which the sanitizers do not make: an
    # empty optional dereferenced, a vector or a string_view indexed past its
    # end, front() of an empty container. libstdc++ reads the first, libc++ the
    # second; each ignores the other's.
    add_compile_definitions(_GLIBCXX_ASSERTIONS _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)
    # The checks are on, so the build says so (cmake/NobionHardening.cmake adds
    # nothing of its own here).
    set(NOBION_HARDENED_STDLIB ON)
  endif()
  message(STATUS "nobion: sanitizers enabled: ${NOBION_SANITIZE}")
endif()
