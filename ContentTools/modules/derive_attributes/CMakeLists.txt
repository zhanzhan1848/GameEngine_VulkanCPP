# Phase 1 derive_attributes module (MikkTSpace adapter).
#
# The derive module rebuilds normals / tangents on a ProcessableMesh after
# other pipeline passes (subdivide, remesh). Tangent derivation uses the
# MikkTSpace algorithm — SMikkTSpaceContext is the bridge struct that
# MikkTSpaceAdapter.cpp sets up. We vendor only mikktspace.h; the .c
# implementation is NOT linked here, so any caller that wants full
# tangent derivation must additionally link upstream mikktspace.c (see
# third_party/mikktspace/README for the current stub state).
#
# The static lib here is therefore a "compile-only" target for the adapter
# skeleton — linking it into ContentTools gives us the API surface, but
# MikkTSpaceAdapter::create_context is a no-op until the .c is added.

set(_derive_sources
    DeriveAttributes.h
    DeriveAttributes.cpp
    MikkTSpaceAdapter.h
    MikkTSpaceAdapter.cpp
)

add_library(contenttools_derive STATIC ${_derive_sources})

target_include_directories(contenttools_derive PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/mikktspace
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
)