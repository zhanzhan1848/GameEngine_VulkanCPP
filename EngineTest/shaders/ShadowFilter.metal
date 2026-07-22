#pragma once

#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"

// Shadow filter: half-res compute pass that produces a visibility texture.
// Deferred lighting reads the result (1 sample) instead of doing multi-tap PCF.
// This avoids Apple Silicon texture bandwidth limits in the main lighting pass.

struct ShadowFilterParams {
    float4x4 inv_view_proj;       // Reconstruct world pos from depth
    float4x4 shadow_vp[2];        // Shadow VP matrices for cascade 0, 1
    float4   light_dir;           // Normalized light direction (for bias)
    float2   texel_size;          // 1.0 / 2048.0
    float2   depth_texel_size;    // 1.0 / (render_width, render_height)
    uint     shadow_quality;      // 0=Hard, 1=PCF, 2=PCSS
    uint     render_width;
    uint     render_height;
    float    _pad0;
};

// --- Shadow sampling (inlined, limited taps for compute bandwidth safety) ---

float pcfShadow(texture2d<float> sm, sampler smp, float3 sc, float2 screenPos,
                float filterRadius, float bias) {
    float shadow = 0.0;
    float noise = fract(IGN_MAGIC.z * fract(dot(screenPos, IGN_MAGIC.xy)));
    float angle = noise * TWO_PI;
    float sa = sin(angle), ca = cos(angle);

    for (int i = 0; i < 8; ++i) {
        float2 offset = POISSON_DISK_16[i];
        float2 rotated = float2(offset.x * ca - offset.y * sa,
                                 offset.x * sa + offset.y * ca);
        float2 uv = sc.xy + rotated * filterRadius;
        float d = sm.sample(smp, uv).r;
        shadow += (sc.z - bias > d) ? 0.0 : 1.0;
    }
    return shadow / 8.0;
}

float pcssShadow(texture2d<float> sm, sampler smp, float3 sc, float2 screenPos,
                 float2 texelSize, float bias, float lightSizeUV) {
    // Blocker search (8 taps)
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    float noise = fract(IGN_MAGIC.z * fract(dot(screenPos, IGN_MAGIC.xy)));
    float angle = noise * TWO_PI;
    float sa = sin(angle), ca = cos(angle);

    for (int i = 0; i < 8; ++i) {
        float2 offset = POISSON_DISK_16[i];
        float2 rotated = float2(offset.x * ca - offset.y * sa,
                                 offset.x * sa + offset.y * ca);
        float2 uv = sc.xy + rotated * lightSizeUV;
        float d = sm.sample(smp, uv).r;
        if (d < sc.z - bias) {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5) return 1.0;

    float penumbraRatio = (sc.z - blockerSum / blockerCount) / sc.z;
    float filterRadius = clamp(penumbraRatio * lightSizeUV, texelSize.x * 2.0, lightSizeUV * 4.0);

    // Filtered PCF (16 taps)
    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i];
        float2 rotated = float2(offset.x * ca - offset.y * sa,
                                 offset.x * sa + offset.y * ca);
        float2 uv = sc.xy + rotated * filterRadius;
        float d = sm.sample(smp, uv).r;
        shadow += (sc.z - bias > d) ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}

float sampleShadowMap(texture2d<float> sm, sampler smp, float3 sc,
                      float2 screenPos, float2 texelSize, float bias,
                      uint quality) {
    if (quality == 0u) {
        float d = sm.sample(smp, sc.xy).r;
        return (sc.z - bias > d) ? 0.0 : 1.0;
    } else if (quality == 1u) {
        return pcfShadow(sm, smp, sc, screenPos, 2.0 * texelSize.x, bias);
    } else {
        return pcssShadow(sm, smp, sc, screenPos, texelSize, bias, 0.005);
    }
}

kernel void shadow_filter_compute(
    uint2 gid [[thread_position_in_grid]],
    constant ShadowFilterParams& params [[buffer(0)]],
    texture2d<float> depthTex [[texture(0)]],
    texture2d<float> shadowMap0 [[texture(1)]],
    texture2d<float> shadowMap1 [[texture(2)]],
    texture2d<float, access::write> visibilityOut [[texture(3)]],
    texture2d<float> normalTex [[texture(4)]]
) {
    uint2 outSize = uint2(visibilityOut.get_width(), visibilityOut.get_height());
    if (any(gid >= outSize)) return;

    float2 fullUV = (float2(gid) * 2.0 + 1.0) / float2(params.render_width, params.render_height);

    constexpr sampler depthSmp(coord::normalized, filter::nearest, address::clamp_to_edge);
    float depth = depthTex.sample(depthSmp, fullUV).r;

    if (depth >= 1.0) {
        visibilityOut.write(float4(1.0), gid);
        return;
    }

    float2 ndc = float2(fullUV.x * 2.0 - 1.0, 1.0 - fullUV.y * 2.0);
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = params.inv_view_proj * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    float bias = 0.003;
    float2 screenPos = float2(gid);
    float visibility = 1.0;

    float4 clip0 = params.shadow_vp[0] * float4(worldPos, 1.0);
    float3 sc0 = clip0.xyz / clip0.w;
    sc0.x = sc0.x * 0.5 + 0.5;
    sc0.y = sc0.y * -0.5 + 0.5;

    if (sc0.x >= 0.0 && sc0.x <= 1.0 && sc0.y >= 0.0 && sc0.y <= 1.0 && sc0.z >= 0.0 && sc0.z <= 1.0) {
        constexpr sampler shadowSmp(coord::normalized, filter::nearest, address::clamp_to_edge);
        visibility = sampleShadowMap(shadowMap0, shadowSmp, sc0, screenPos,
                                     params.texel_size, bias, params.shadow_quality);
    } else {
        float4 clip1 = params.shadow_vp[1] * float4(worldPos, 1.0);
        float3 sc1 = clip1.xyz / clip1.w;
        sc1.x = sc1.x * 0.5 + 0.5;
        sc1.y = sc1.y * -0.5 + 0.5;

        if (sc1.x >= 0.0 && sc1.x <= 1.0 && sc1.y >= 0.0 && sc1.y <= 1.0 && sc1.z >= 0.0 && sc1.z <= 1.0) {
            constexpr sampler shadowSmp(coord::normalized, filter::nearest, address::clamp_to_edge);
            visibility = sampleShadowMap(shadowMap1, shadowSmp, sc1, screenPos,
                                         params.texel_size, bias, params.shadow_quality);
        }
    }

    visibilityOut.write(float4(visibility), gid);
}
