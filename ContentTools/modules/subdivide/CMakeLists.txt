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

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(contenttools_subdivide PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    >
)
