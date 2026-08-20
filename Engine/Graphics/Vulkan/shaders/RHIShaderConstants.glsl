// RHIShaderConstants.glsl — Phase 0.1 port of RHIShaderConstants.metal.
// Engine-wide constants shared by all Vulkan GLSL shaders. Include via
// `#include "RHIShaderConstants.glsl"` (build_spv.sh inlines via awk).
//
// Mirrors Engine/Graphics/RHI/Shaders/RHIShaderConstants.metal line-for-line.

#ifndef RHIS_SHADER_CONSTANTS_GLSL
#define RHIS_SHADER_CONSTANTS_GLSL

// ================================================================================================
// Math constants
// ================================================================================================
const float PI    = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647693;
const float INV_PI = 0.31830988618379067154;
const float HALF_PI = 1.57079632679489661923;

// ================================================================================================
// Shadow constants
// ================================================================================================

// VSM minimum variance — guards against numerical instability in Chebyshev upper bound.
const float VSM_MIN_VARIANCE = 0.00002;

// Poisson disk samples (16-tap). Mirror Metal POISSON_DISK_16 order.
const vec2 POISSON_DISK_16[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// Interleaved Gradient Noise magic numbers (Jorge Jimenez 2012).
const vec3 IGN_MAGIC = vec3(0.06711056, 0.00583715, 52.9829189);

#endif // RHIS_SHADER_CONSTANTS_GLSL
