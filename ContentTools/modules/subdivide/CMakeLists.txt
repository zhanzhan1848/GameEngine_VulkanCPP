# Phase 1 subdivide module (PMP Loop / CatmullClark).
#
# Subdivide.{h,cpp} wraps pmp::loop_subdivision / pmp::catmull_clark_subdivision.
# Each PMP call is one subdivision step; Params.levels applies the scheme
# that many times. Loop on a triangle mesh multiplies face count by ~4
# per step.
#
# PMP is brought in via add_subdirectory at the parent ContentTools/
# CMakeLists.txt scope; we link the `pmp` target.

set(_subdivide_sources
    Subdivide.h
    Subdivide.cpp
)

add_library(contenttools_subdivide STATIC ${_subdivide_sources})

target_include_directories(contenttools_subdivide PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/pmp-library/src
)

target_link_libraries(contenttools_subdivide PUBLIC pmp)

target_compile_options(contenttools_subdivide PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-unused-but-set-variable
)
