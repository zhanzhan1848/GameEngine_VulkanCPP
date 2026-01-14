#pragma once

#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics::rhi {

// 参考 Engine/Graphics/Metal/shaders/CommonTypes.metal
// 确保与 Shader 中的结构体对齐一致 (16字节对齐)

struct GlobalShaderData
{
    math::m4x4 view;
    math::m4x4 projection;
    math::m4x4 invProjection;
    math::m4x4 viewProjection;
    math::m4x4 previousViewProjection;
    math::m4x4 invViewProjection;

    math::v4 cameraPositionAndViewWidth;   // xyz: position, w: viewWidth
    math::v4 cameraDirectionAndViewHeight; // xyz: direction, w: viewHeight

    uint32_t numDirectionalLights;
    float deltaTime;
    float frameCount;
    float padding;
};

struct PerObjectData
{
    math::m4x4 world;
    math::m4x4 invWorld;
    math::m4x4 worldViewProjection;
};

// 对应 Metal 中的 LightParameters
struct LightParameters
{
    math::v3 position;
    float intensity;

    math::v3 direction;
    float range;

    math::v3 color;
    float cosUmbra;       // Cosine of the half angle of umbra (Spot Light)

    math::v3 attenuation;
    float cosPenumbra;    // Cosine of the half angle of penumbra (Spot Light)

    // math::v3 padding;
    // uint32_t type;        // 0: Directional, 1: Point, 2: Spot (如果不分离 Directional)
};

// 对应 Metal 中的 DirectionalLightParameters
struct DirectionalLightParameters
{
    math::m4x4 lightMVP; // Shadow Mapping Matrix

    math::v4 directionAndIntensity; // xyz: direction, w: intensity
    
    math::v4 color; // rgb: color, a: padding
};

// 用于 Forward Renderer 的统一光照缓冲区
struct ForwardLightBuffer {
    uint32_t directionalLightCount;
    uint32_t punctualLightCount;
    uint32_t padding[2];
    DirectionalLightParameters directionalLights[4]; // Max 4 directional lights
    LightParameters lights[128]; // Max 128 punctual lights
};

static_assert((sizeof(PerObjectData) % 16) == 0, "PerObjectData must be 16-byte aligned");
static_assert((sizeof(LightParameters) % 16) == 0, "LightParameters must be 16-byte aligned");
static_assert((sizeof(DirectionalLightParameters) % 16) == 0, "DirectionalLightParameters must be 16-byte aligned");

} // namespace primal::graphics::rhi
