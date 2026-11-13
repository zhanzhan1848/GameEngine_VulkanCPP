# Phase 1 pipeline orchestration layer (AssetPipeline + SceneBlobWriter).
#
# AssetPipeline.{h,cpp} chains repair/remesh/subdivide/uvatlas/lod/meshlet
# per the enable flags in Config. SceneBlobWriter.{h,cpp} serializes the
# per-mesh output into the scene_data.buffer binary layout that 4 downstream
# consumers depend on.
#
# This static lib depends on all module libs + the common SDF helper
# (for the optional enable_sdf path). The parent ContentTools CMakeLists
# links it into the dylib.

set(_pipeline_sources
    AssetPipeline.h
    AssetPipeline.cpp
    SceneBlobWriter.h
    SceneBlobWriter.cpp
    SceneBlobReader.h
    SceneBlobReader.cpp
    PackedMeshDecoder.h
    PackedMeshDecoder.cpp
    Validator.h
    Validator.cpp
)

add_library(contenttools_pipeline STATIC ${_pipeline_sources})

target_include_directories(contenttools_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
)

target_link_libraries(contenttools_pipeline PUBLIC
    contenttools_repair
    contenttools_remesh
    contenttools_subdivide
    contenttools_derive
    contenttools_uvatlas
    contenttools_lod
    contenttools_meshlet
    contenttools_collision
)

# R3（Windows 首编）：-Wno-*/-stdlib=libc++ 等 GCC/clang 专属选项经
# MSVC 解析为 /W + 非数字参数 → D8021 致命错误。非 MSVC 编译器才下发
# （对齐父目录 ContentTools/CMakeLists.txt 的 genexp 分支先例）。
target_compile_options(contenttools_pipeline PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
    >
)
