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

    u32 numDirectionalLights;
    u32 numPunctualLights;
    float deltaTime;
    float frameCount;

    u32 renderMode; // 0=NoEffects, 1=ShadowOnly, 2=ShadowAndIBL, 3=Full
    u32 enableIBL;  // 0 = skip IBL ambient (MeshletNoIBL), 1 = apply (Meshlet). Mirrors the WGSL GlobalShaderData slot formerly named _pad0.
    u32 enableDDGI; // 0 = skip DDGI indirect (default), 1 = apply indirect lighting from binding 13. Mode 10 sets this to 1.
    u32 _pad_before_jitter; // WGSL uniform layout requires vec2 at 8-byte align; Mac simd::float2 already has 8-byte align (auto-pads here), WASM float[2] has 4-byte align (needs explicit pad)
    math::v2 jitterOffset; // TAA subpixel jitter (clip-space units, applied in PBR vertex) — offset 448 on both platforms
    // Live-tunable debug params (WASM sidebar). 4 shader-side floats replace the
    // former _pad_after_jitter[2]. Total grows from 464→480 (16-byte aligned).
    float debug_directLightBoost;     // offset 456 — replaces DeferredLighting_Meshlet.wgsl const 2.0
    float debug_iblStrength;          // offset 460 — replaces const 0.2
    float debug_ddgiIndirectWeight;   // offset 464 — replaces const 1.0
    float debug_exposure;             // offset 468 — replaces const 1.8
    float _pad_after_debug[2];        // offset 472-479 — round to 480 (WebGPU uniform 16-byte multiple)
};

static_assert(sizeof(GlobalShaderData) == 480, "GlobalShaderData must be 480 bytes — adds 16-byte debug params block after jitter (was 464)");

struct PerObjectData
{
    math::m4x4 world;
    math::m4x4 invWorld;
    math::m4x4 worldViewProjection;
    math::m4x4 prevWorldViewProjection; // Previous frame WVP for velocity computation
    math::v4 sh_coeffs[9]; // SH9 Color coefficients (RGB + padding/alpha)
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

    int lightType;        // 1: Point, 2: Spot
    int shadowIndex;      // -1: No shadow, >=0: Index
    float padding;        // Padding to 16 bytes
    math::m4x4 viewProjection; // Spot Light Shadow Matrix
};

// 对应 Metal 中的 DirectionalLightParameters
struct DirectionalLightParameters
{
    math::m4x4 viewProjections[4]; // Cascade ViewProjection matrices
    math::v4 splits;               // Cascade split distances
    
    math::v4 directionAndIntensity; // xyz: direction, w: intensity
    
    math::v4 colorAndShadow; // rgb: color, a: shadow enabled (float 1.0 or 0.0)
};

// 用于 Forward Renderer 的统一光照缓冲区
struct ForwardLightBuffer {
    u32 directionalLightCount;
    u32 punctualLightCount;
    u32 padding[2];
    DirectionalLightParameters directionalLights[4]; // Max 4 directional lights
    LightParameters lights[128]; // Max 128 punctual lights
};

static_assert(sizeof(PerObjectData) == 400, "PerObjectData size mismatch with WGSL");
static_assert(sizeof(LightParameters) == 192, "LightParameters must be 192 bytes (matches WGSL PunctualLightParameters)");
static_assert(sizeof(DirectionalLightParameters) == 304, "DirectionalLightParameters must be 304 bytes");
static_assert(sizeof(ForwardLightBuffer) == 25808, "ForwardLightBuffer size mismatch");
static_assert(offsetof(LightParameters, position) == 0, "LightParameters.position offset mismatch");
static_assert(offsetof(LightParameters, direction) == 32, "LightParameters.direction offset mismatch");
static_assert(offsetof(LightParameters, color) == 64, "LightParameters.color offset mismatch");
static_assert(offsetof(LightParameters, attenuation) == 96, "LightParameters.attenuation offset mismatch");
static_assert(offsetof(LightParameters, viewProjection) == 128, "LightParameters.viewProjection offset mismatch");

} // namespace primal::graphics::rhi
