# MetalShaderValidation.cmake
#
# Validates all Metal shaders compile successfully at build time using xcrun.
# Reports ALL errors in one pass (does not stop at first failure).
#
# Usage:
#   cmake --build build --target ValidateMetalShaders
#
# Enable auto-validation on every build:
#   cmake -DVALIDATE_METAL_SHADERS_ON_BUILD=ON ..

option(VALIDATE_METAL_SHADERS_ON_BUILD
    "Run Metal shader validation on every build" OFF)

if(NOT APPLE)
    return()
endif()

# ---------------------------------------------------------------------------
# Include paths for xcrun (covers all shader groups)
# ---------------------------------------------------------------------------

set(_METAL_INCLUDE_FLAGS
    -I "${CMAKE_SOURCE_DIR}/Engine/Graphics/Metal/shaders"
    -I "${CMAKE_SOURCE_DIR}/Engine/Graphics/Metal/shaders/Lumen"
    -I "${CMAKE_SOURCE_DIR}/Engine/Graphics/Metal/shaders/Nanite"
    -I "${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Shaders"
    -I "${CMAKE_SOURCE_DIR}/EngineTest/shaders"
    -I "${CMAKE_SOURCE_DIR}/EngineTest/shaders/Lumen"
)

# ---------------------------------------------------------------------------
# Collect all Metal shader files
# ---------------------------------------------------------------------------

file(GLOB_RECURSE _ALL_METAL_SHADERS
    "${CMAKE_SOURCE_DIR}/Engine/Graphics/Metal/shaders/*.metal"
    "${CMAKE_SOURCE_DIR}/Engine/Graphics/RHI/Shaders/*.metal"
    "${CMAKE_SOURCE_DIR}/EngineTest/shaders/*.metal"
)

# ---------------------------------------------------------------------------
# Skip list — files excluded from validation
#
# Reasons:
#   Header-only: no entry point, can't compile standalone
#   Common.h:    PI redefinition between CommonConstants.metal and
#                CommonFunction.metal (engine's ResolveIncludes deduplicates,
#                but xcrun doesn't)
#   WIP:         Work-in-progress shaders with known issues (auto& address
#                space, struct mismatches)
# ---------------------------------------------------------------------------

set(_SKIP_NAMES
    # Header-only
    CommonTypes.metal CommonConstants.metal CommonFunction.metal
    BuildinMaterial.metal SDFBox.metal SDFBasicShape.metal
    DDGIVolumeData.metal DDGISample.metal ProbeConfidence.metal
    StaticProbeSampling.metal SurfaceCacheData.metal
    RHIShaderCommon.metal RHIShaderConstants.metal RHIShaderPBR.metal
    RHIShaderTypes.metal IBL_Hammersley.metal SH_Common.metal
    # Common.h legacy (PI redefinition)
    FullScreenTriangle.metal DepthPassShader.metal
    Particle.metal ParticleCompute.metal
    buildin_shader.metal SSAOShader.metal SSGIShader.metal
    PostProcess.metal
    # WIP (auto& address space, struct mismatch)
    DDGICardRadianceAvg.metal DDGIProbeIrradianceFromCards.metal
    SurfaceCacheLightCull.metal SurfaceCacheLightEval.metal
)

# ---------------------------------------------------------------------------
# Filter out skipped files
# ---------------------------------------------------------------------------

set(_VALIDATE_LIST "")
foreach(_S ${_ALL_METAL_SHADERS})
    get_filename_component(_NAME ${_S} NAME)
    list(FIND _SKIP_NAMES ${_NAME} _IDX)
    if(_IDX GREATER_EQUAL 0)
        continue()
    endif()
    list(APPEND _VALIDATE_LIST ${_S})
endforeach()

# ---------------------------------------------------------------------------
# Generate cmake validation script (runs at configure time)
# ---------------------------------------------------------------------------

set(_SCRIPT "${CMAKE_BINARY_DIR}/validate_metal_shaders.cmake")
set(_AIR_DIR "${CMAKE_BINARY_DIR}/metal_validation")

file(MAKE_DIRECTORY ${_AIR_DIR})

# Script header
file(WRITE ${_SCRIPT} "# Auto-generated Metal shader validation script\n")
file(APPEND ${_SCRIPT} "cmake_minimum_required(VERSION 3.20)\n\n")
file(APPEND ${_SCRIPT} "set(_AIR_DIR \"${_AIR_DIR}\")\n")
file(APPEND ${_SCRIPT} "set(_FAIL 0)\n")
file(APPEND ${_SCRIPT} "set(_PASS 0)\n")
file(APPEND ${_SCRIPT} "set(_FAILED \"\")\n\n")

# One execute_process per shader (keeps output interleaved with progress)
foreach(_S ${_VALIDATE_LIST})
    file(RELATIVE_PATH _REL "${CMAKE_SOURCE_DIR}" ${_S})
    string(REPLACE "/" "_" _AIR_NAME "${_REL}")
    string(REPLACE "." "_" _AIR_NAME "${_AIR_NAME}")

    file(APPEND ${_SCRIPT} "execute_process(\n")
    file(APPEND ${_SCRIPT} "    COMMAND xcrun -sdk macosx metal -c \"${_S}\" ${_METAL_INCLUDE_FLAGS} -o \"\${_AIR_DIR}/${_AIR_NAME}.air\" -Wno-unused-variable\n")
    file(APPEND ${_SCRIPT} "    OUTPUT_QUIET\n")
    file(APPEND ${_SCRIPT} "    ERROR_VARIABLE _ERR\n")
    file(APPEND ${_SCRIPT} "    RESULT_VARIABLE _RC\n")
    file(APPEND ${_SCRIPT} ")\n")
    file(APPEND ${_SCRIPT} "if(_RC EQUAL 0)\n")
    file(APPEND ${_SCRIPT} "    math(EXPR _PASS \"\${_PASS} + 1\")\n")
    file(APPEND ${_SCRIPT} "    message(STATUS \"  OK  ${_REL}\")\n")
    file(APPEND ${_SCRIPT} "else()\n")
    file(APPEND ${_SCRIPT} "    math(EXPR _FAIL \"\${_FAIL} + 1\")\n")
    file(APPEND ${_SCRIPT} "    string(APPEND _FAILED \"\\n  ${_REL}\")\n")
    file(APPEND ${_SCRIPT} "    message(\"\")\n")
    file(APPEND ${_SCRIPT} "    message(\"FAIL ${_REL}\")\n")
    file(APPEND ${_SCRIPT} "    message(\"\${_ERR}\")\n")
    file(APPEND ${_SCRIPT} "    message(\"\")\n")
    file(APPEND ${_SCRIPT} "endif()\n\n")
endforeach()

# Summary
file(APPEND ${_SCRIPT} "message(\"\")\n")
file(APPEND ${_SCRIPT} "if(_FAIL GREATER 0)\n")
file(APPEND ${_SCRIPT} "    message(FATAL_ERROR \"Metal shader validation FAILED: \${_FAIL} error(s):\${_FAILED}\")\n")
file(APPEND ${_SCRIPT} "else()\n")
file(APPEND ${_SCRIPT} "    message(STATUS \"All \${_PASS} Metal shaders passed validation\")\n")
file(APPEND ${_SCRIPT} "endif()\n")

# ---------------------------------------------------------------------------
# Build target
# ---------------------------------------------------------------------------

add_custom_target(ValidateMetalShaders
    COMMAND ${CMAKE_COMMAND} -P ${_SCRIPT}
    COMMENT "Validating Metal shaders with xcrun..."
)

if(VALIDATE_METAL_SHADERS_ON_BUILD)
    add_dependencies(Engine ValidateMetalShaders)
endif()
