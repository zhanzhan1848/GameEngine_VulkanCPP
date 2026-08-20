// RHIShaderFunctions.glsl — Phase 0.4 port of RHIShaderFunctions.metal.
// General shader helpers: full-screen triangle, IG noise, VSM Chebyshev bound,
// Reinhard tonemap, and PCF/VSM shadow sampling helpers for 2D-array and
// cube-array shadow maps.
//
// Differences from Metal original:
//   - GLSL uses combined sampler types: `sampler2DArrayShadow`,
//     `sampler2DArray`, `samplerCubeArrayShadow`, `samplerCubeArray`.
//     Metal's separate `depth2d_array + sampler` pairs collapse into these.
//   - GetFullScreenTrianglePosUV does NOT flip Y. Vulkan with positive
//     viewport height has NDC +Y at framebuffer BOTTOM, so NDC(-1,-1) lands
//     at framebuffer TOP-LEFT which matches UV (0,0) directly. Metal flips
//     Y to compensate for its texture-coordinate convention; Vulkan does not.
//   - `out` parameters replace Metal's `thread T&` references.
//
// Constants (PI, TWO_PI, IGN_MAGIC, POISSON_DISK_16, VSM_MIN_VARIANCE) come
// from RHIShaderConstants.glsl, included by the parent shader via build_spv.sh
// awk inline.

#ifndef RHIS_SHADER_FUNCTIONS_GLSL
#define RHIS_SHADER_FUNCTIONS_GLSL

#ifdef RHI_ENABLE_PBR
#include "RHIShaderPBR.glsl"
#endif

// ================================================================================================
// 工具函数
// ================================================================================================

// Generates the position and UV for a fullscreen triangle (vertexID 0,1,2).
// Reference: RHIShaderFunctions.metal:32-40.
void GetFullScreenTrianglePosUV(uint vertexID, out vec4 outPos, out vec2 outUV) {
    vec2 pos;
    pos.x = (vertexID == 2) ? 3.0 : -1.0;
    pos.y = (vertexID == 1) ? 3.0 : -1.0;

    outPos = vec4(pos, 0.0, 1.0);
    outUV = pos * 0.5 + 0.5;
    // No Y flip in Vulkan — NDC +Y is at framebuffer bottom with positive
    // viewport height, so NDC(-1,-1) lands at framebuffer TOP-LEFT which
    // matches UV (0,0) directly.
}

// Interleaved Gradient Noise for dithering (Jorge Jimenez 2012).
float InterleavedGradientNoise(vec2 screenPos) {
    return fract(IGN_MAGIC.z * fract(dot(screenPos, IGN_MAGIC.xy)));
}

// Chebyshev Upper Bound for VSM. `t` is the current depth (dist to light).
float ChebyshevUpperBound(vec2 moments, float t, float minVariance) {
    if (t <= moments.x) return 1.0;

    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);

    float d = t - moments.x;
    float p_max = variance / (variance + d * d);

    // Reduce light bleeding.
    return smoothstep(0.05, 1.0, p_max);
}

// Simple Reinhard Tone Mapping.
vec3 ToneMapReinhard(vec3 color) {
    return color / (color + vec3(1.0));
}

// ================================================================================================
// Shadow sampling functions
// ================================================================================================

// PCF shadow sampling (2D array, comparison sampler, 5x5 kernel).
// Reference: RHIShaderFunctions.metal:80-90.
float SampleShadowPCF(sampler2DArrayShadow shadowMap,
                      vec3 shadowCoord, float layer,
                      vec2 texelSize, float bias) {
    float shadow = 0.0;
    int radius = 2; // 5x5 PCF
    for (int x = -radius; x <= radius; ++x) {
        for (int y = -radius; y <= radius; ++y) {
            vec2 offset = vec2(x, y) * texelSize;
            shadow += texture(shadowMap,
                              vec4(shadowCoord.xy + offset, layer,
                                   shadowCoord.z - bias));
        }
    }
    return shadow / ((2.0 * float(radius) + 1.0) * (2.0 * float(radius) + 1.0));
}

// VSM shadow sampling (2D array, 16-tap Poisson disk, IG-noise rotation).
// Reference: RHIShaderFunctions.metal:95-118.
float SampleShadowVSM(sampler2DArray shadowMap,
                      vec3 shadowCoord, float layer,
                      float minVariance, vec2 screenPos) {
    vec2 moments = vec2(0.0);
    float spread = 0.0015; // Filter radius

    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * TWO_PI;
    float s = sin(angle);
    float c = cos(angle);

    for (int i = 0; i < 16; ++i) {
        vec2 diskOffset = POISSON_DISK_16[i];
        vec2 rotatedOffset = vec2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        vec2 offset = rotatedOffset * spread;
        moments += texture(shadowMap,
                           vec3(shadowCoord.xy + offset, uint(layer))).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, shadowCoord.z, minVariance);
}

// Point shadow sampling (cube array, comparison sampler).
// Reference: RHIShaderFunctions.metal:123-126.
float SamplePointShadow(samplerCubeArrayShadow shadowMap,
                        vec3 dir, float dist,
                        float near, float far, float bias, int layer) {
    float depth = far / (far - near) - (far * near) / (dist * (far - near));
    return texture(shadowMap, vec4(dir, layer), depth - bias);
}

// Point shadow sampling (cube array, VSM, 16-tap Poisson disk).
// Reference: RHIShaderFunctions.metal:131-160.
float SamplePointShadowVSM(samplerCubeArray shadowMap,
                           vec3 dir, float dist,
                           float near, float far, float minVariance,
                           int layer, vec2 screenPos) {
    float linearDepth = (dist - near) / (far - near);

    vec3 L = normalize(dir);

    vec3 up = abs(L.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 right = normalize(cross(up, L));
    up = cross(L, right);

    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * TWO_PI;
    float s = sin(angle);
    float c = cos(angle);

    vec2 moments = vec2(0.0);
    float spread = 0.003;

    for (int i = 0; i < 16; ++i) {
        vec2 diskOffset = POISSON_DISK_16[i];
        vec2 rotatedOffset = vec2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        vec3 offset = (right * rotatedOffset.x + up * rotatedOffset.y) * spread;
        moments += texture(shadowMap, vec4(L + offset, uint(layer))).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, linearDepth, minVariance);
}

#endif // RHIS_SHADER_FUNCTIONS_GLSL
