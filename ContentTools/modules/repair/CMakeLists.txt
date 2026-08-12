# Phase 1 repair module (PMP Library).
#
# Repair.{h,cpp} wrap pmp::SurfaceMesh cleanup operations: degenerate face
# removal, border stitching, hole filling, face orientation consistency.
#
# PMP is brought in via add_subdirectory(third_party/pmp-library) at the
# parent ContentTools/CMakeLists.txt scope; we just link against the target.

set(_repair_sources
    Repair.h
    Repair.cpp
)

# Build as a STATIC library so the AssetPipeline (M8) and the parent ContentTools
# SHARED lib can both link this in. Phase 1 modules are statically composed —
# no per-module dylib.
add_library(contenttools_repair STATIC ${_repair_sources})

target_include_directories(contenttools_repair PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # ContentTools/ for "common/..."
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/pmp-library/src
    ${CMAKE_SOURCE_DIR}/third_party/eigen
)

target_link_libraries(contenttools_repair PUBLIC pmp)

target_compile_options(contenttools_repair PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
)

# Suppress PMP's own warnings (it's a vendored third-party).
if(APPLE)
    target_compile_options(contenttools_repair PRIVATE -Wno-unused-but-set-variable)
endif()
