# Phase 1 collision module (Unity-Technologies VHACD).
#
# VHACD's own CMakeLists.txt assumes a custom `CMAKE_COMMON_INC` wrapper
# that doesn't compose with add_subdirectory from the engine's tree. We
# instead compile VHACD sources directly into contenttools_collision,
# mirroring how meshoptimizer is integrated into ContentTools itself.
#
# VHACD-ASYNC.cpp is excluded — we use the synchronous CreateVHACD() entry
# point and never use the async thread-pool path.

set(_collision_module_sources
    Collision.h
    Collision.cpp
)

set(_vhacd_root ${CMAKE_SOURCE_DIR}/third_party/VHACD/src/VHACD_Lib)

if(NOT EXISTS "${_vhacd_root}/public/VHACD.h" OR
   NOT EXISTS "${_vhacd_root}/src/VHACD.cpp")
    message(FATAL_ERROR "VHACD submodule missing at ${_vhacd_root}. "
        "Run 'git submodule update --init --recursive'.")
endif()

set(_vhacd_sources
    ${_vhacd_root}/src/VHACD.cpp
    ${_vhacd_root}/src/btAlignedAllocator.cpp
    ${_vhacd_root}/src/btConvexHullComputer.cpp
    ${_vhacd_root}/src/FloatMath.cpp
    ${_vhacd_root}/src/vhacdICHull.cpp
    ${_vhacd_root}/src/vhacdManifoldMesh.cpp
    ${_vhacd_root}/src/vhacdMesh.cpp
    ${_vhacd_root}/src/vhacdRaycastMesh.cpp
    ${_vhacd_root}/src/vhacdVolume.cpp
)

add_library(contenttools_collision STATIC
    ${_collision_module_sources}
    ${_vhacd_sources}
)

target_include_directories(contenttools_collision PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${_vhacd_root}/public
    ${_vhacd_root}/inc
)

# VHACD needs NO_OPENCL / NO_OPENMP to skip OpenCL/OpenMP probing at
# compile time. Without these, headers may try to use platform features
# we don't link against.
target_compile_definitions(contenttools_collision PRIVATE
    NO_OPENCL
    NO_OPENMP
)

target_compile_options(contenttools_collision PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-unknown-pragmas
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
    -Wno-shift-count-overflow
    -Wno-implicit-int-conversion
    -Wno-double-promotion
    -Wno-missing-field-initializers
    -Wno-zero-as-null-pointer-constant
    -Wno-old-style-cast
    -Wno-float-equal
    -Wno-switch-enum
    -Wno-covered-switch-default
    -Wno-format-nonliteral
    -Wno-exit-time-destructors
    -Wno-global-constructors
    -Wno-c++98-compat-pedantic
    -Wno-c++11-compat
    -Wno-cast-align
    -Wno-conversion
    -Wno-missing-noreturn
    -Wno-padded
    -Wno-reserved-id-macro
    -Wno-shadow
    -Wno-shadow-field
    -Wno-undef
    -Wno-unused-macros
    -Wno-used-but-marked-unused
    -Wno-weak-vtables
)
