# Phase 1 meshlet module (meshoptimizer).
#
# Meshlet.{h,cpp} wraps meshopt_buildMeshlets with the runtime contract
# 256-vertex / 128-triangle cap (Engine/Graphics/Nanite/MeshletSynthesis.h).
# Geometry.cpp:291-301's legacy 64/124 path is preserved untouched.
#
# meshopt is compiled into the ContentTools dylib via the parent CMake.
# This static lib links against the dylib at the parent level (symbols
# resolved at link time).

set(_meshlet_sources
    Meshlet.h
    Meshlet.cpp
)

add_library(contenttools_meshlet STATIC ${_meshlet_sources})

target_include_directories(contenttools_meshlet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src
)

target_compile_options(contenttools_meshlet PRIVATE
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
