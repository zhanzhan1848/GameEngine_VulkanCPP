# Phase 1 lod module (meshoptimizer).
#
# Lod.{h,cpp} wraps meshopt_simplify with multi-level recursion. Each level
# simplifies the previous one to `ratio` of its index count, then compacts
# the vertex pool (positions/normals/tangents/colors/uv_sets through the
# same permutation).
#
# meshoptimizer is already compiled into the ContentTools dylib via the
# parent CMakeLists.txt. We re-include its headers here and link the
# meshoptimizer symbols via the ContentTools SHARED lib at the parent level.
# For the standalone static lib used by tests, meshopt symbols are resolved
# at link time against ContentTools.dylib.

set(_lod_sources
    Lod.h
    Lod.cpp
)

add_library(contenttools_lod STATIC ${_lod_sources})

target_include_directories(contenttools_lod PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src
)

target_compile_options(contenttools_lod PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
)
