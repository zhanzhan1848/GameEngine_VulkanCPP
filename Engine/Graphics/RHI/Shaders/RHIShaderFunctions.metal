#pragma once

#include <metal_stdlib>
using namespace metal;

#include "RHIShaderConstants.metal"

// ================================================================================================
// 按需引入模块
// ================================================================================================

#ifdef RHI_ENABLE_PBR
#include "RHIShaderPBR.metal"
#endif

/**
 * @file RHIShaderFunctions.metal
 * @brief 通用着色器函数库
 * @details 包含几何生成、噪声、色调映射和阴影采样函数
 */

// ================================================================================================
// 工具函数
// ================================================================================================

/**
 * @brief 生成全屏三角形的顶点位置和UV
 * @param vertexID 顶点索引 (0, 1, 2)
 * @param outPos 输出裁剪空间位置
 * @param outUV 输出UV坐标 (0..1, 左上角原点)
 */
inline void GetFullScreenTrianglePosUV(uint vertexID, thread float4& outPos, thread float2& outUV) {
    float2 pos;
    pos.x = (vertexID == 2) ? 3.0 : -1.0;
    pos.y = (vertexID == 1) ? 3.0 : -1.0;
    
    outPos = float4(pos, 0.0, 1.0);
    outUV = pos * 0.5 + 0.5;
    outUV.y = 1.0 - outUV.y; // Flip Y to match Metal texture coordinates (0,0 at top-left)
}

/**
 * @brief Interleaved Gradient Noise for dithering
 */
inline float InterleavedGradientNoise(float2 position_screen) {
    return fract(IGN_MAGIC.z * fract(dot(position_screen, IGN_MAGIC.xy)));
}

/**
 * @brief Chebyshev Upper Bound for VSM
 */
inline float ChebyshevUpperBound(float2 moments, float t, float minVariance) {
    // t is the current depth (dist to light)
    if (t <= moments.x) return 1.0;
    
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, minVariance);
    
    float d = t - moments.x;
    float p_max = variance / (variance + d * d);
    
    // Reduce light bleeding
    return smoothstep(0.05, 1.0, p_max);
}

/**
 * @brief Simple Reinhard Tone Mapping
 */
inline float3 ToneMapReinhard(float3 color) {
    return color / (color + float3(1.0));
}

// ================================================================================================
// 阴影采样函数
// ================================================================================================

/**
 * @brief PCF 阴影采样 (Array)
 */
inline float SampleShadowPCF(depth2d_array<float> shadowMap, sampler shadowSampler, float3 shadowCoord, float layer, float2 texelSize, float bias) {
    float shadow = 0.0;
    int radius = 2; // 5x5 PCF
    for (int x = -radius; x <= radius; ++x) {
        for (int y = -radius; y <= radius; ++y) {
            float2 offset = float2(x, y) * texelSize;
            shadow += shadowMap.sample_compare(shadowSampler, float2(shadowCoord.xy + offset), int(layer), shadowCoord.z - bias);
        }
    }
    return shadow / ((2.0 * radius + 1.0) * (2.0 * radius + 1.0));
}

/**
 * @brief VSM 阴影采样 (Array, Poisson Disk)
 */
inline float SampleShadowVSM(texture2d_array<float> shadowMap, sampler shadowSampler, float3 shadowCoord, float layer, float minVariance, float2 screenPos) {
    float2 moments = float2(0.0);
    float spread = 0.0015; // Filter radius

    // Random Rotation
    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * TWO_PI;
    float s = sin(angle);
    float c = cos(angle);

    // Accumulate moments from samples
    for (int i = 0; i < 16; ++i) {
        float2 diskOffset = POISSON_DISK_16[i];
        float2 rotatedOffset = float2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        float2 offset = rotatedOffset * spread;
        moments += shadowMap.sample(shadowSampler, shadowCoord.xy + offset, uint(layer)).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, shadowCoord.z, minVariance);
}

/**
 * @brief Point Shadow Helper (Cube, Standard)
 */
inline float SamplePointShadow(depthcube_array<float> shadowMap, sampler shadowSampler, float3 dir, float dist, float near, float far, float bias, int layer) {
    float depth = far / (far - near) - (far * near) / (dist * (far - near));
    return shadowMap.sample_compare(shadowSampler, dir, uint(layer), depth - bias);
}

/**
 * @brief Point Shadow Helper (Cube, VSM)
 */
inline float SamplePointShadowVSM(texturecube_array<float> shadowMap, sampler shadowSampler, float3 dir, float dist, float near, float far, float minVariance, int layer, float2 screenPos) {
    float linearDepth = (dist - near) / (far - near);
    
    float3 L = normalize(dir);
    
    float3 up = abs(L.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right = normalize(cross(up, L));
    up = cross(L, right);
    
    float noise = InterleavedGradientNoise(screenPos);
    float angle = noise * TWO_PI;
    float s = sin(angle);
    float c = cos(angle);
    
    float2 moments = float2(0.0);
    float spread = 0.003; 

    for (int i = 0; i < 16; ++i) {
        float2 diskOffset = POISSON_DISK_16[i];
        float2 rotatedOffset = float2(
            diskOffset.x * c - diskOffset.y * s,
            diskOffset.x * s + diskOffset.y * c
        );
        float3 offset = (right * rotatedOffset.x + up * rotatedOffset.y) * spread;
        moments += shadowMap.sample(shadowSampler, L + offset, uint(layer)).xy;
    }
    moments /= 16.0;

    return ChebyshevUpperBound(moments, linearDepth, minVariance);
}
