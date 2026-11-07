# nobion_set_warnings(<target>)
# Strict warnings, and the standard library's own checks
# (cmake/NobionHardening.cmake), for first-party targets only (third-party
# deps are untouched).
function(nobion_set_warnings target)
  if(NOBION_HARDENING_DEFINITIONS)
    target_compile_definitions(${target} PRIVATE ${NOBION_HARDENING_DEFINITIONS})
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(NOBION_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      -Woverloaded-virtual -Wnull-dereference -Wimplicit-fallthrough)
    if(NOBION_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
