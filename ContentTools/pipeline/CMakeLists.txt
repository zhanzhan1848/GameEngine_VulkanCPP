# Phase 1 pipeline module (M8: AssetPipeline orchestration).
#
# The pipeline composes the M2-M10 modules (ProcessableMesh, repair, remesh,
# subdivide, derive_attributes, uvatlas, lod, meshlet, collision) and writes
# the SceneBlobWriter output. Sources depend on every other module's headers,
# so this static lib is linked after all peer modules are built.

set(_pipeline_sources
    AssetPipeline.h
    AssetPipeline.cpp
    PackedMeshDecoder.h
    PackedMeshDecoder.cpp
    SceneBlobReader.h
    SceneBlobReader.cpp
    SceneBlobWriter.h
    SceneBlobWriter.cpp
    Validator.h
    Validator.cpp
)

add_library(contenttools_pipeline STATIC ${_pipeline_sources})

target_include_directories(contenttools_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine
    ${CMAKE_SOURCE_DIR}/Engine/Common
    ${CMAKE_SOURCE_DIR}/Engine/Utilities
)

target_compile_options(contenttools_pipeline PRIVATE
    -std=c++17
    -stdlib=libc++
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-shorten-64-to-32
    -Wno-double-promotion
)

# Xcode 26.5 SDK's <simd/vector_make.h> uses std::make_tuple at line 8499
# but only includes <tuple> at line 5922. Force-include <tuple> before
# any other system header so all TUs that pull in <simd/simd.h> have
# std::make_tuple in scope.
target_compile_options(contenttools_pipeline PRIVATE -include tuple)

# pipeline composes every peer module — declare link deps so the linker
# can resolve ProcessableMesh/repair/remesh/etc symbols referenced by
# AssetPipeline.cpp.
target_link_libraries(contenttools_pipeline PUBLIC
    contenttools_repair
    contenttools_remesh
    contenttools_subdivide
    contenttools_uvatlas
    contenttools_lod
    contenttools_meshlet
    contenttools_collision
    contenttools_derive
)