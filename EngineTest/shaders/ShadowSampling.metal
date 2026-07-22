#pragma once

#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"

// Shadow sampling functions for R32_Float textures (blitted from D32_Float on Metal TBDR).
// All functions use manual depth comparison — hardware sample_compare requires depth2d format.

// Single-tap hard shadow
float SampleShadowHard(texture2d<float> shadowMap, sampler sm,
                       float3 shadowCoord, float bias) {
    float d = shadowMap.sample(sm, shadowCoord.xy).r;
    return (shadowCoord.z - bias > d) ? 0.0 : 1.0;
}

// 16-tap Poisson disk PCF with IGN-based rotation to eliminate banding
float SampleShadowPCF_Poisson(texture2d<float> shadowMap, sampler sm,
                               float3 shadowCoord, float2 screenPos,
                               float filterRadius, float bias) {
    float shadow = 0.0;
    float noise = fract(IGN_MAGIC.z * fract(dot(screenPos, IGN_MAGIC.xy)));
    float angle = noise * TWO_PI;
    float s = sin(angle), c = cos(angle);

    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i];
        float2 rotated = float2(offset.x * c - offset.y * s,
                                 offset.x * s + offset.y * c);
        float2 sampleUV = shadowCoord.xy + rotated * filterRadius;
        float d = shadowMap.sample(sm, sampleUV).r;
        shadow += (shadowCoord.z - bias > d) ? 0.0 : 1.0;
    }
    return shadow / 16.0;
}

// PCSS: blocker search + adaptive penumbra PCF
// Two-pass: find average blocker depth, then filter with penumbra-sized kernel
float SampleShadowPCSS(texture2d<float> shadowMap, sampler sm,
                       float3 shadowCoord, float2 screenPos,
                       float2 texelSize, float bias,
                       float lightSizeUV) {
    // Blocker search (16 taps)
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    float searchRadius = lightSizeUV;

    float noise = fract(IGN_MAGIC.z * fract(dot(screenPos, IGN_MAGIC.xy)));
    float angle = noise * TWO_PI;
    float s = sin(angle), c = cos(angle);

    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i];
        float2 rotated = float2(offset.x * c - offset.y * s,
                                 offset.x * s + offset.y * c);
        float2 sampleUV = shadowCoord.xy + rotated * searchRadius;
        float d = shadowMap.sample(sm, sampleUV).r;
        if (d < shadowCoord.z - bias) {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }

    if (blockerCount < 0.5) return 1.0; // No blockers = fully lit

    float avgBlockerDepth = blockerSum / blockerCount;

    // Penumbra estimation
    float penumbraRatio = (shadowCoord.z - avgBlockerDepth) / shadowCoord.z;
    float filterRadius = penumbraRatio * lightSizeUV;
    filterRadius = clamp(filterRadius, texelSize.x * 2.0, lightSizeUV * 4.0);

    // Filtered PCF (32 taps — 16 Poisson + 16 rotated copies)
    float shadow = 0.0;
    for (int i = 0; i < 32; ++i) {
        float2 offset = POISSON_DISK_16[i % 16];
        float2 rotated = float2(offset.x * c - offset.y * s,
                                 offset.x * s + offset.y * c);
        if (i >= 16) rotated = float2(rotated.y, -rotated.x);
        float2 sampleUV = shadowCoord.xy + rotated * filterRadius;
        float d = shadowMap.sample(sm, sampleUV).r;
        shadow += (shadowCoord.z - bias > d) ? 0.0 : 1.0;
    }
    return shadow / 32.0;
}

// Quality-dispatched shadow sampling
// shadowQuality: 0=Hard, 1=PCF_16, 2=PCSS
float GetShadowFiltered(texture2d<float> shadowMap, sampler sm,
                        float3 shadowCoord, float2 screenPos,
                        float2 texelSize, float bias,
                        uint shadowQuality) {
    if (shadowQuality == 0u) {
        return SampleShadowHard(shadowMap, sm, shadowCoord, bias);
    } else if (shadowQuality == 1u) {
        return SampleShadowPCF_Poisson(shadowMap, sm, shadowCoord, screenPos,
                                       2.0 * texelSize.x, bias);
    } else {
        return SampleShadowPCSS(shadowMap, sm, shadowCoord, screenPos,
                                texelSize, bias, 0.005);
    }
}
