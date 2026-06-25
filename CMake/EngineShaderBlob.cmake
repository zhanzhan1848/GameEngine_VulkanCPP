# EngineShaderBlob.cmake
#
# Builds the engine shaders metallib and the legacy packed blob that
# Engine/Graphics/Metal/MetalShader.cpp::load_engine_shaders expects.
# Driven by build_engine_shader_blob.py which handles the actual
# xcrun metal/metallib calls and binary packing.
#
# Adds one target: BuildEngineShaderBlob
# Outputs to ${CMAKE_BINARY_DIR}/shaders/metal/shaders.metallib
# (the cwd-relative path that load_engine_shaders uses when launched
# from Darwin/<Config>/PrimalEditor_Avalonia)

if(NOT APPLE)
    return()
endif()

set(_ENGINE_SHADERS_DIR "${CMAKE_SOURCE_DIR}/Engine/Graphics/Metal/shaders")
set(_ENGINE_BLOB_OUT_DIR "${CMAKE_BINARY_DIR}/shaders/metal")
set(_ENGINE_BLOB_TEMP     "${CMAKE_BINARY_DIR}/_shader_blob_temp")
set(_ENGINE_METALLIB      "${_ENGINE_BLOB_OUT_DIR}/shaders.metallib")
set(_ENGINE_PACKED_BLOB   "${_ENGINE_BLOB_OUT_DIR}/shaders.metallib.packed")

file(MAKE_DIRECTORY ${_ENGINE_BLOB_OUT_DIR})

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT ${_ENGINE_METALLIB} ${_ENGINE_PACKED_BLOB}
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/CMake/build_engine_shader_blob.py"
        --source-dir "${_ENGINE_SHADERS_DIR}"
        --metallib-out "${_ENGINE_METALLIB}"
        --packed-out "${_ENGINE_PACKED_BLOB}"
        --air-temp-dir "${_ENGINE_BLOB_TEMP}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/CMake/build_engine_shader_blob.py"
        "${_ENGINE_SHADERS_DIR}/EditorShaders.metal"
    COMMENT "Building engine shaders metallib and packed legacy blob"
    VERBATIM
)

add_custom_target(BuildEngineShaderBlob
    DEPENDS ${_ENGINE_PACKED_BLOB}
    COMMENT "Engine shaders packed legacy blob"
)

# Make the editor copy the blob into its working directory at build time
# so the engine can find it. This runs as part of the editor's PostBuild.
add_custom_command(
    OUTPUT "${CMAKE_SOURCE_DIR}/Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/shaders.metallib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_ENGINE_PACKED_BLOB}"
        "${CMAKE_SOURCE_DIR}/Darwin/PrimalEditor_Avalonia/DebugEditor/shaders/metal/shaders.metallib"
    DEPENDS ${_ENGINE_PACKED_BLOB}
    COMMENT "Copying engine shaders blob to editor output"
    VERBATIM
)
