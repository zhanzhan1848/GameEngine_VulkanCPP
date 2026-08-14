# Phase 1 derive_attributes module.
#
# Recomputes normals and tangents on ProcessableMesh. Used by AssetPipeline
# auto-cascade after subdivide (default), and standalone via CLI --derive.
#
# Vendor mikktspace.c is compiled as part of this static lib (single TU,
# zlib license, no external deps). C source compiled with C++ compiler
# to avoid dual-language toolchain.

set(_derive_module_sources
    DeriveAttributes.h
    DeriveAttributes.cpp
    MikkTSpaceAdapter.h
    MikkTSpaceAdapter.cpp
)

set(_mikktspace_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/mikktspace)

if(NOT EXISTS "${_mikktspace_dir}/mikktspace.c" OR
   NOT EXISTS "${_mikktspace_dir}/mikktspace.h")
    message(FATAL_ERROR "mikktspace vendor files missing at ${_mikktspace_dir}. "
        "See plan M11.1 for download instructions.")
endif()

set(_mikktspace_sources
    ${_mikktspace_dir}/mikktspace.c
    ${_mikktspace_dir}/mikktspace.h
)

# mikktspace.c is a C source — compile with C, not C++ (project default).
set_source_files_properties(${_mikktspace_dir}/mikktspace.c PROPERTIES
    LANGUAGE C
    COMPILE_OPTIONS "-std=c99;-O3"
)

add_library(contenttools_derive STATIC
    ${_derive_module_sources}
    ${_mikktspace_sources}
)

target_include_directories(contenttools_derive PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${_mikktspace_dir}
)

target_compile_options(contenttools_derive PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
    -Wno-implicit-fallthrough
    -Wno-shift-count-overflow
)
