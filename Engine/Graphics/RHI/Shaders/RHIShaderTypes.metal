#pragma once

#include <metal_stdlib>
using namespace metal;

/**
 * @file RHIShaderTypes.metal
 * @brief 通用着色器数据结构定义
 * @details 包含全局Uniform定义、光照结构体等，与 C++ 层 Engine/Graphics/RHI/Core/RHIShaderCommon.h 保持一致
 */

// ================================================================================================
// 基础数据结构
// ================================================================================================

/**
 * @brief 全局着色器数据 (Buffer 11 通常)
 */
struct GlobalShaderData
{
    float4x4 view;
    float4x4 projection;
    float4x4 invProjection;
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4x4 invViewProjection;

    float4 cameraPositionAndViewWidth;   // xyz: position, w: viewWidth
    float4 cameraDirectionAndViewHeight; // xyz: direction, w: viewHeight

    uint numDirectionalLights;
    uint numPunctualLights;
    float deltaTime;
    float frameCount;
};

/**
 * @brief每对象数据 (Buffer 10 通常)
 */
struct PerObjectData
{
    float4x4 world;
    float4x4 invWorld;
    float4x4 worldViewProjection;
    float4 sh_coeffs[9];
};

/**
 * @brief 单个光源参数
 */
struct LightParameters
{
    float3 position;
    float intensity;

    float3 direction;
    float range;

    float3 color;
    float cosUmbra;

    float3 attenuation;
    float cosPenumbra;

    int lightType;        // 1: Point, 2: Spot
    int shadowIndex;      // -1: No shadow, >=0: Index
    float padding;
    
    float4x4 viewProjection;
};

/**
 * @brief 方向光参数 (支持 CSM)
 */
struct DirectionalLightParameters
{
    float4x4 viewProjections[4]; // Cascade ViewProjection matrices
    float4 splits;               // Cascade split distances
    
    float4 directionAndIntensity; // xyz: direction, w: intensity
    
    float4 colorAndShadow; // rgb: color, a: shadow enabled
};

/**
 * @brief 前向渲染光照缓冲区 (Buffer 12 通常)
 */
struct ForwardLightBuffer {
    uint directionalLightCount;
    uint punctualLightCount;
    uint padding[2];
    DirectionalLightParameters directionalLights[4];
    LightParameters lights[128];
};

/**
 * @brief 材质 Uniforms (示例)
 */
struct MaterialUniforms {
    float4 color;
};

/**
 * @brief PBR 材质参数
 */
struct PBRMaterialParameters {
    float4 baseColorFactor;
    float3 emissiveFactor;
    float roughnessFactor;
    float metallicFactor;
    float normalScale;
    float occlusionStrength;
    float padding;
    
    // 纹理开关 (0: 关, 1: 开)
    int hasBaseColorTexture;
    int hasNormalTexture;
    int hasMetallicRoughnessTexture;
    int hasOcclusionTexture;
    int hasEmissiveTexture;
    int padding2[3];
};
