# nobion::reduction. Sources are globbed so units landing in parallel only add
# files under src/ and include/nobion/reduction/ and never edit this list.
file(GLOB_RECURSE NOBION_REDUCTION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(nobion_reduction STATIC ${NOBION_REDUCTION_SOURCES})
add_library(nobion::reduction ALIAS nobion_reduction)

target_include_directories(nobion_reduction PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(nobion_reduction PUBLIC cxx_std_20)
target_link_libraries(nobion_reduction PUBLIC nobion::core)

nobion_set_warnings(nobion_reduction)

# Parity with Python's per-operation rounding (spec 4.7): never fuse a * b + c
# into an FMA. PUBLIC because UFloat's arithmetic (combine, variance) is inline
# in a public header: every consumer must compile it unfused too, or the
# linker may pick a contracted copy (spec 7). MSVC's default /fp:precise does
# not contract across statements and needs nothing here.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nobion_reduction PUBLIC -ffp-contract=off)
endif()
