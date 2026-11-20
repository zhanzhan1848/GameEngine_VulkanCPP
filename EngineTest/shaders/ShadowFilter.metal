#pragma once

#include <metal_stdlib>
using namespace metal;
#include "RHIShaderConstants.metal"

// Shadow filter: half-res compute pass that produces a visibility texture.
// Deferred lighting reads the result (1 sample) instead of doing multi-tap PCF.
// This avoids Apple Silicon texture bandwidth limits in the main lighting pass.

struct ShadowFilterParams {
    float4x4 inv_view_proj;       // Reconstruct world pos from depth
    float4x4 shadow_vp[4];        // Match the shared four-cascade CPU upload.
    float4   light_dir;           // Normalized light direction (for bias)
    float2   texel_size;          // 1.0 / 2048.0
    float2   depth_texel_size;    // 1.0 / (render_width, render_height)
    uint     shadow_quality;      // 0=Hard, 1=PCF, 2=PCSS
    uint     render_width;
    uint     render_height;
    float    sun_tan_half_angle;
    float4   cascade_params[4];   // texel world size, depth span, extent, reserved
};
static_assert(sizeof(ShadowFilterParams) == 432, "Shadow filter CPU/MSL ABI");
static_assert(__builtin_offsetof(ShadowFilterParams, light_dir) == 320, "Shadow light ABI");
static_assert(__builtin_offsetof(ShadowFilterParams, render_width) == 356, "Shadow extent ABI");

// Frozen Vulkan ShadowFilter.comp sampling sequence, kept local so other
// Metal passes retain their existing shared constants.
constant float3 SHADOW_IGN = float3(0.7548776662, 0.5698402909, 0.2659582193);
constant float2 SHADOW_DISK[16] = {
    float2(-0.5274373, -0.7259060), float2(0.8218882, -0.1694079),
    float2(-0.7959683, 0.5603005), float2(0.5697909, 0.7113788),
    float2(-0.1583544, -0.8058264), float2(0.7826165, 0.5406202),
    float2(-0.6480213, -0.1694079), float2(0.55, -0.45),
    float2(-0.3, 0.7), float2(0.7, -0.7), float2(-0.7, 0.3),
    float2(0.3, 0.8), float2(-0.6, -0.5), float2(0.85, 0.4),
    float2(-0.2, 0.9), float2(0.4, -0.3)
};

// --- Shadow sampling ---

float pcfShadow(texture2d<float> sm, sampler smp, float3 sc, float2 screenPos,
                float filterRadius, float bias) {
    float shadow = 0.0;
    float noise = fract(SHADOW_IGN.z * fract(dot(screenPos, SHADOW_IGN.xy)));
    float angle = noise * TWO_PI;
    float sa = sin(angle), ca = cos(angle);

    for (int i = 0; i < 8; ++i) {
        float2 offset = SHADOW_DISK[i];
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
    float noise = fract(SHADOW_IGN.z * fract(dot(screenPos, SHADOW_IGN.xy)));
    float angle = noise * TWO_PI;
    float sa = sin(angle), ca = cos(angle);

    for (int i = 0; i < 8; ++i) {
        float2 offset = SHADOW_DISK[i];
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

    float penumbraRatio = (sc.z - blockerSum / blockerCount) / max(sc.z, 1e-4);
    float filterRadius = clamp(penumbraRatio * lightSizeUV, texelSize.x * 2.0, lightSizeUV * 4.0);

    // Filtered PCF (16 taps)
    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        float2 offset = SHADOW_DISK[i];
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
                      uint quality, float lightSizeUV) {
    if (quality == 0u) {
        float d = sm.sample(smp, sc.xy).r;
        return (sc.z - bias > d) ? 0.0 : 1.0;
    } else if (quality == 1u) {
        return pcfShadow(sm, smp, sc, screenPos, 2.0 * texelSize.x, bias);
    } else {
        return pcssShadow(sm, smp, sc, screenPos, texelSize, bias, lightSizeUV);
    }
}

float3 projectToCascade(float4x4 vp, float3 worldPos) {
    float4 clip = vp * float4(worldPos, 1.0);
    float3 sc = clip.xyz / max(clip.w, 1e-6);
    sc.xy = sc.xy * float2(0.5, -0.5) + 0.5;
    return sc;
}

bool cascadeContains(float3 sc) {
    return all(sc >= 0.0) && all(sc <= 1.0);
}

float cascadeBiasNDC(constant ShadowFilterParams& p, uint c, float slope, float ndl) {
    float s = min(slope, ndl > 0.0 ? 3.0 : 4.0);
    return p.cascade_params[c].x * (0.5 + 2.0 * s) / max(p.cascade_params[c].y, 1e-3);
}

float3 cascadeNormalOffset(constant ShadowFilterParams& p, uint c, float3 n, float slope, float ndl) {
    float k = ndl > 0.0 ? min(1.0 + slope, 4.0) : 1.0 + 2.0 * min(slope, 4.0);
    return n * (p.cascade_params[c].x * k);
}

float pcssLightSizeUV(constant ShadowFilterParams& p, uint c, float z) {
    if (p.sun_tan_half_angle <= 0.0) return 0.012;
    float radius = z * max(p.cascade_params[c].y, 1e-3) * p.sun_tan_half_angle
                 / max(p.cascade_params[c].z * 2.0, 1e-3);
    return clamp(radius, p.texel_size.x, 0.05);
}

kernel void shadow_filter_compute(
    uint2 gid [[thread_position_in_grid]],
    constant ShadowFilterParams& params [[buffer(0)]],
    texture2d<float> depthTex [[texture(0)]],
    texture2d<float> shadowMap0 [[texture(1)]],
    texture2d<float> shadowMap1 [[texture(2)]],
    texture2d<float, access::write> visibilityOut [[texture(3)]],
    texture2d<float> normalTex [[texture(4)]],
    texture2d<float> shadowMap2 [[texture(7)]],
    texture2d<float> shadowMap3 [[texture(8)]],
    texture2d<float, access::write> translucentOut [[texture(9)]],
    sampler shadowSmp [[sampler(6)]]
) {
    uint2 outSize = uint2(visibilityOut.get_width(), visibilityOut.get_height());
    if (any(gid >= outSize)) return;
    translucentOut.write(float4(0.0), gid);

    // 伪影1 探针（PRIMAL_SHADOW_WHITE=1 → C++ 置 shadow_quality=255）：
    // 全白 visibility（无阴影）。楔形亮斑消失 = 坐实 shadow vis 路径。
    if (params.shadow_quality == 255u) {
        visibilityOut.write(float4(1.0), gid);
        return;
    }

    float2 fullUV = (float2(gid) * 2.0 + 1.0) / float2(params.render_width, params.render_height);

    float depth = depthTex.sample(shadowSmp, fullUV).r;

    if (depth >= 1.0) {
        visibilityOut.write(float4(1.0), gid);
        return;
    }

    // C1.1 三段直出诊断（PRIMAL_SFD_MODE 编译期选择，R8 [0,1] 编码）：
    //   2 = 采样的 gbuffer depth（sky 已被上方 guard 置 1）
    //   3/4/5 = cascade0 接收器 sc0.z / sc0.x / sc0.y
    // 与 Docs/2026-09-09 计划 §C1.1 的解析真值逐像素对照定位断点。
#ifdef PRIMAL_SFD_MODE
    #if PRIMAL_SFD_MODE == 2
        visibilityOut.write(float4(depth), gid); return;
    #endif
#endif

    // 模式 6：shadow map 原始内容（按屏幕 fullUV 直读 cascade0）——
    // 镜像/缩放/幽灵几何一目了然（Round 9 遗留的"map 全图读回"替代路径）。
#ifdef PRIMAL_SFD_MODE
    #if PRIMAL_SFD_MODE == 6
        {
            constexpr sampler mapSmp(coord::normalized, filter::nearest, address::clamp_to_edge);
            visibilityOut.write(float4(shadowMap0.sample(mapSmp, fullUV).r), gid);
            return;
        }
    #endif
#endif

    // 模式 8：shadow map 全图直读——vis 纹理 UV（0..1）映射 map UV，
    // 输出整张 2048² cascade0 深度图的灰度缩览（含 clear 值/几何带）。
#ifdef PRIMAL_SFD_MODE
    #if PRIMAL_SFD_MODE == 8
        {
            constexpr sampler mapSmp(coord::normalized, filter::linear, address::clamp_to_edge);
            float2 mapUV = (float2(gid) + 0.5) / float2(outSize);
            visibilityOut.write(float4(shadowMap0.sample(mapSmp, mapUV).r), gid);
            return;
        }
    #endif
#endif

    float2 ndc = float2(fullUV.x * 2.0 - 1.0, 1.0 - fullUV.y * 2.0);
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = params.inv_view_proj * clipPos;
    float3 worldPos = worldPos4.xyz / max(worldPos4.w, 1e-6);

    float3 N = normalTex.sample(shadowSmp, fullUV).rgb * 2.0 - 1.0;
    if (dot(N, N) < 1e-4) N = float3(0.0, -1.0, 0.0);
    N = normalize(N);
    float ndl = dot(N, -normalize(params.light_dir.xyz));
    float slope = clamp(sqrt(max(0.0, 1.0 - ndl * ndl)) / max(abs(ndl), 0.25), 0.0, 4.0);
    float2 screenPos = float2(gid);
    float visibility = 1.0;

    float4 clip0 = params.shadow_vp[0] * float4(worldPos, 1.0);
    float3 sc0 = clip0.xyz / clip0.w;
    sc0.x = sc0.x * 0.5 + 0.5;
    sc0.y = sc0.y * -0.5 + 0.5;

#ifdef PRIMAL_SFD_MODE
    #if PRIMAL_SFD_MODE == 3
        visibilityOut.write(float4(saturate(sc0.z), 0.0, 0.0, 1.0), gid); return;
    #elif PRIMAL_SFD_MODE == 4
        visibilityOut.write(float4(saturate(sc0.x), 0.0, 0.0, 1.0), gid); return;
    #elif PRIMAL_SFD_MODE == 5
        visibilityOut.write(float4(saturate(sc0.y), 0.0, 0.0, 1.0), gid); return;
    #elif PRIMAL_SFD_MODE == 7
        // 在 sc0.xy 处直读 cascade0 map 深度——与模式 3 的接收器 sc0.z
        // 逐像素对照，直接量化 Round 11 的"深度尺度差"假设。
        {
            constexpr sampler mapSmp(coord::normalized, filter::nearest, address::clamp_to_edge);
            float d = shadowMap0.sample(mapSmp, sc0.xy).r;
            visibilityOut.write(float4(saturate(d), 0.0, 0.0, 1.0), gid);
            return;
        }
    #endif
#endif

    // Same runtime diagnostic quality values as the frozen Vulkan filter.
    if (params.shadow_quality >= 3u && params.shadow_quality <= 7u) {
        float3 sc = projectToCascade(params.shadow_vp[0], worldPos + cascadeNormalOffset(params, 0u, N, slope, ndl));
        float bias = cascadeBiasNDC(params, 0u, slope, ndl);
        float value = 0.0;
        if (params.shadow_quality == 7u) {
            value = saturate(ndl * 0.5 + 0.5);
        } else if (params.shadow_quality == 4u) {
            value = saturate(projectToCascade(params.shadow_vp[0], worldPos).z);
        } else if (params.shadow_quality == 5u) {
            if (cascadeContains(sc)) value = sc.z - bias > shadowMap0.sample(shadowSmp, sc.xy).r ? 0.0 : 1.0;
        } else if (params.shadow_quality == 6u) {
            if (cascadeContains(sc)) {
                float count = 0.0;
                for (uint i = 0; i < 8; ++i) {
                    float d = shadowMap0.sample(shadowSmp, sc.xy + SHADOW_DISK[i] * 0.05).r;
                    if (d < sc.z - bias) count += 1.0;
                }
                value = saturate(count * 0.5);
            }
        } else if (cascadeContains(sc)) {
            value = shadowMap0.sample(shadowSmp, sc.xy).r;
        }
        visibilityOut.write(float4(value), gid);
        return;
    }

    // Select the first covering cascade, with Vulkan's world-scale normal
    // offset, slope bias and physical sun radius. Native Metal texture slots
    // match the existing non-Vulkan binding table in ShadowMapModule.cpp.
    float transmission = 0.0;
    for (uint c = 0; c < 4; ++c) {
        float3 sc = projectToCascade(params.shadow_vp[c], worldPos + cascadeNormalOffset(params, c, N, slope, ndl));
        if (!cascadeContains(sc)) continue;
        float bias = cascadeBiasNDC(params, c, slope, ndl);
        float radius = pcssLightSizeUV(params, c, sc.z);
        float occ;
        switch (c) {
            case 0:
                visibility = sampleShadowMap(shadowMap0, shadowSmp, sc, screenPos, params.texel_size, bias, params.shadow_quality, radius);
                occ = shadowMap0.sample(shadowSmp, sc.xy).r; break;
            case 1:
                visibility = sampleShadowMap(shadowMap1, shadowSmp, sc, screenPos, params.texel_size, bias, params.shadow_quality, radius);
                occ = shadowMap1.sample(shadowSmp, sc.xy).r; break;
            case 2:
                visibility = sampleShadowMap(shadowMap2, shadowSmp, sc, screenPos, params.texel_size, bias, params.shadow_quality, radius);
                occ = shadowMap2.sample(shadowSmp, sc.xy).r; break;
            default:
                visibility = sampleShadowMap(shadowMap3, shadowSmp, sc, screenPos, params.texel_size, bias, params.shadow_quality, radius);
                occ = shadowMap3.sample(shadowSmp, sc.xy).r; break;
        }
        float thickness = max((sc.z - bias) - occ, 0.0) * params.cascade_params[c].y;
        transmission = exp(-thickness * 1.5) * max(-ndl, 0.0) * (1.0 - visibility);
        break;
    }
    translucentOut.write(float4(clamp(transmission, 0.0, 1.0)), gid);

    visibilityOut.write(float4(visibility), gid);
}
