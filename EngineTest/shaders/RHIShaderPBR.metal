#pragma once

#include <metal_stdlib>
using namespace metal;

#include "RHIShaderConstants.metal"

/**
 * @file RHIShaderPBR.metal
 * @brief 基于物理的渲染 (PBR) 核心函数库
 * @details 包含 Cook-Torrance BRDF 的各个分量：NDF, Geometry, Fresnel
 */

// ================================================================================================
// PBR 基础函数 (Cook-Torrance BRDF)
// ================================================================================================

/**
 * @brief 法线分布函数 (NDF) - Trowbridge-Reitz GGX
 * @param N 法线向量
 * @param H 半程向量
 * @param roughness 粗糙度
 * @return 分布概率
 */
inline float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / max(denom, 0.0000001); // 避免除零
}

/**
 * @brief 几何遮蔽函数 (Geometry) - Schlick-GGX (Direct Lighting)
 * @param NdotV 法线与视角的点积
 * @param roughness 粗糙度
 * @return 几何遮蔽因子 [0, 1]
 */
inline float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / max(denom, 0.0000001);
}

/**
 * @brief 几何遮蔽函数 (Geometry) - Schlick-GGX (IBL)
 * @param NdotV 法线与视角的点积
 * @param roughness 粗糙度
 * @return 几何遮蔽因子 [0, 1]
 */
inline float GeometrySchlickGGX_IBL(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / max(denom, 0.0000001);
}

/**
 * @brief 几何遮蔽函数 (Geometry) - Smith Method
 * @details 结合了视线方向和光照方向的遮蔽
 * @param N 法线向量
 * @param V 视角向量
 * @param L 光照向量
 * @param roughness 粗糙度
 * @return 综合几何遮蔽因子
 */
inline float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

/**
 * @brief 几何遮蔽函数 (Geometry) - Smith Method (IBL)
 * @details 结合了视线方向和光照方向的遮蔽，使用 IBL 的 k 值
 * @param N 法线向量
 * @param V 视角向量
 * @param L 光照向量
 * @param roughness 粗糙度
 * @return 综合几何遮蔽因子
 */
inline float GeometrySmith_IBL(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX_IBL(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX_IBL(NdotL, roughness);

    return ggx1 * ggx2;
}

/**
 * @brief 菲涅尔方程 (Fresnel) - Schlick Approximation
 * @param cosTheta 视角与半程向量的点积 (dot(H, V)) 或 法线与视角 (dot(N, V))
 * @param F0 基础反射率 (F0)
 * @return 菲涅尔反射系数
 */
inline float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

/**
 * @brief 菲涅尔方程 (Fresnel) - Schlick Approximation with Roughness
 * @details 用于环境光照 (IBL)，考虑粗糙度对菲涅尔效应的影响
 * @param cosTheta 法线与视角的点积
 * @param F0 基础反射率
 * @param roughness 粗糙度
 * @return 菲涅尔反射系数
 */
inline float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

/**
 * @brief 计算 Cook-Torrance BRDF 的镜面反射部分
 * @param N 法线
 * @param V 视角方向
 * @param L 光照方向
 * @param roughness 粗糙度
 * @param F0 基础反射率
 * @return Specular BRDF 贡献 (未乘以 NdotL)
 */
inline float3 SpecularBRDF(float3 N, float3 V, float3 L, float roughness, float3 F0) {
    float3 H = normalize(V + L);
    
    float D = DistributionGGX(N, H, roughness);   
    float G = GeometrySmith(N, V, L, roughness);      
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
       
    float3 nominator = D * G * F; 
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float denominator = 4.0 * NdotV * NdotL + 0.001; // 0.001 避免除零
    
    return nominator / denominator;
}
