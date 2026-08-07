# Phase 1 uvatlas module (xatlas).
#
# xatlas is compiled directly from third_party/xatlas/source/xatlas/ — no
# vendoring step. xatlas.cpp is self-contained (only STL dependencies) and
# compiling it here avoids drifting against a vendored 10k-line copy.
# `xatlas.h` is the public header; `xatlas_c.h` is the C ABI (unused here).
#
# UvAtlas.{h,cpp} wraps Create/AddMesh/Generate/Destroy. Per Run() call:
#   - ProcessableMesh IR -> xatlas::MeshDecl
#   - xatlas re-indexes vertices (seam insertion); output Vertex.xref maps
#     back to the input vertex array.
#   - harvest_output pulls positions/normals/tangents/colors AND every
#     existing uv_sets entry through xref, then writes generated UVs to
#     the matching-purpose UVSet.

set(_uvatlas_module_sources
    UvAtlas.h
    UvAtlas.cpp
)

set(_xatlas_dir ${CMAKE_SOURCE_DIR}/third_party/xatlas/source/xatlas)

if(NOT EXISTS "${_xatlas_dir}/xatlas.cpp" OR
   NOT EXISTS "${_xatlas_dir}/xatlas.h")
    message(FATAL_ERROR "xatlas submodule missing at ${_xatlas_dir}. "
        "Run 'git submodule update --init --recursive'.")
endif()

# Compile xatlas.cpp as part of this static lib (single TU). xatlas has no
# nested thirdparty deps beyond STL headers, so this is sufficient.
set(_xatlas_sources
    ${_xatlas_dir}/xatlas.cpp
    ${_xatlas_dir}/xatlas.h
)

add_library(contenttools_uvatlas STATIC
    ${_uvatlas_module_sources}
    ${_xatlas_sources}
)

target_include_directories(contenttools_uvatlas PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${_xatlas_dir}
)

target_compile_options(contenttools_uvatlas PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-shift-count-overflow
    -Wno-shorten-64-to-32
)
