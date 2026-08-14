// RHIShaderPBR.glsl — Phase 0.3 port of RHIShaderPBR.metal.
// Cook-Torrance BRDF helpers — NDF (GGX), Geometry (Smith + Schlick-GGX),
// Fresnel (Schlick + roughness-aware), and combined SpecularBRDF.
//
// 1:1 line-for-line port of Engine/Graphics/RHI/Shaders/RHIShaderPBR.metal.
// Constants come from RHIShaderConstants.glsl (PI), included by the parent
// shader via build_spv.sh awk inline.
//
// Usage: `#include "RHIShaderPBR.glsl"` after RHIShaderConstants.glsl.

#ifndef RHIS_SHADER_PBR_GLSL
#define RHIS_SHADER_PBR_GLSL

// ================================================================================================
// PBR 基础函数 (Cook-Torrance BRDF)
// ================================================================================================

// 法线分布函数 (NDF) - Trowbridge-Reitz GGX
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / max(denom, 0.0000001);
}

// 几何遮蔽函数 (Geometry) - Schlick-GGX (Direct Lighting)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / max(denom, 0.0000001);
}

// 几何遮蔽函数 (Geometry) - Schlick-GGX (IBL)
float GeometrySchlickGGX_IBL(float NdotV, float roughness) {
    float a = roughness;
    float k = (a * a) / 2.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / max(denom, 0.0000001);
}

// 几何遮蔽函数 (Geometry) - Smith Method (Direct Lighting)
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// 几何遮蔽函数 (Geometry) - Smith Method (IBL)
float GeometrySmith_IBL(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX_IBL(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX_IBL(NdotL, roughness);

    return ggx1 * ggx2;
}

// 菲涅尔方程 (Fresnel) - Schlick Approximation
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// 菲涅尔方程 (Fresnel) - Schlick Approximation with Roughness (IBL)
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
         * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// Cook-Torrance BRDF 的镜面反射部分 (未乘以 NdotL)
vec3 SpecularBRDF(vec3 N, vec3 V, vec3 L, float roughness, vec3 F0) {
    vec3 H = normalize(V + L);

    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 nominator = D * G * F;
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float denominator = 4.0 * NdotV * NdotL + 0.001;

    return nominator / denominator;
}

#endif // RHIS_SHADER_PBR_GLSL
