# Phase 1 remesh module (PMP uniform/adaptive + libigl qslim).
#
# Remesh.{h,cpp} wrap two decimation paths:
#   - pmp::uniform_remeshing / pmp::adaptive_remeshing (header-only via PMP)
#   - igl::qslim (header-only libigl core)
#
# libigl is used in header-only mode (no IGL_STATIC_LIBRARY) — just include
# path, no add_subdirectory. PMP is brought in via add_subdirectory at the
# parent ContentTools/CMakeLists.txt scope; we link the `pmp` target.

set(_remesh_sources
    Remesh.h
    Remesh.cpp
)

add_library(contenttools_remesh STATIC ${_remesh_sources})

target_include_directories(contenttools_remesh PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/third_party/pmp-library/src
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/libigl/include
)

target_link_libraries(contenttools_remesh PUBLIC pmp)

target_compile_options(contenttools_remesh PRIVATE
    -std=c++17
    -stdlib=libc++
    # Disable -ffast-math (inherited from the top-level add_compile_options)
    # because qslim's cost function relies on std::numeric_limits<double>::infinity()
    # and std::isinf comparisons. -ffast-math enables -ffinite-math-only which
    # makes those checks always-false and silently breaks the decimation loop.
    -fno-fast-math
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-unused-but-set-variable
)

if(APPLE)
    target_compile_options(contenttools_remesh PRIVATE -Wno-unused-but-set-variable)
endif()
