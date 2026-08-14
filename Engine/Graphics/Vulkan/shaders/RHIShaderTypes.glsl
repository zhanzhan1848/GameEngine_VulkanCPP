// RHIShaderTypes.glsl — Phase 0.2 port of RHIShaderTypes.metal.
// Mirrors C++ Engine/Graphics/RHI/Core/RHIShaderCommon.h structs EXACTLY
// (not the older Metal RHIShaderTypes.metal which is missing prevWorldViewProjection
// + debug params). std140 layout — every vec3 padded to 16B via trailing float/vec1.
//
// Sizes verified via static_asserts in RHIShaderCommon.h:
//   GlobalShaderData                = 480 bytes
//   PerObjectData                   = 400 bytes
//   LightParameters                 = 192 bytes
//   DirectionalLightParameters      = 304 bytes
//   ForwardLightBuffer              = 25808 bytes
//
// Use `layout(std140)` on the UBO declaration.

#ifndef RHIS_SHADER_TYPES_GLSL
#define RHIS_SHADER_TYPES_GLSL

// ================================================================================================
// GlobalShaderData — 480 bytes (matches C++ RHIShaderCommon.h:11-42)
// ================================================================================================
struct GlobalShaderData {
    mat4 view;                          // 0
    mat4 projection;                    // 64
    mat4 invProjection;                 // 128
    mat4 viewProjection;                // 192
    mat4 previousViewProjection;        // 256
    mat4 invViewProjection;             // 320

    vec4 cameraPositionAndViewWidth;    // 384: xyz=position, w=viewWidth
    vec4 cameraDirectionAndViewHeight;  // 400: xyz=direction, w=viewHeight

    uint numDirectionalLights;          // 416
    uint numPunctualLights;             // 420
    float deltaTime;                    // 424
    float frameCount;                   // 428

    uint renderMode;                    // 432: 0=NoEffects,1=ShadowOnly,2=ShadowAndIBL,3=Full
    uint enableIBL;                     // 436
    uint enableDDGI;                    // 440
    uint _pad_before_jitter;            // 444

    vec2 jitterOffset;                  // 448: TAA subpixel jitter
    float debug_directLightBoost;       // 456
    float debug_iblStrength;            // 460
    float debug_ddgiIndirectWeight;     // 464
    float debug_exposure;               // 468
    float _pad_after_debug_0;           // 472
    float _pad_after_debug_1;           // 476
};
// Total: 480 bytes.

// ================================================================================================
// PerObjectData — 400 bytes (matches C++ RHIShaderCommon.h:44-51)
// ================================================================================================
struct PerObjectData {
    mat4 world;                  // 0
    mat4 invWorld;               // 64
    mat4 worldViewProjection;    // 128
    mat4 prevWorldViewProjection;// 192: previous frame WVP for velocity
    vec4 sh_coeffs[9];           // 256: SH9 color coefficients (RGB + padding/alpha). 9*16=144 bytes
};
// Total: 400 bytes.

// ================================================================================================
// LightParameters — 192 bytes (matches C++ RHIShaderCommon.h:54-72)
// ================================================================================================
struct LightParameters {
    vec3 position;        // 0
    float intensity;      // 12

    vec3 direction;       // 16
    float range;          // 28

    vec3 color;           // 32
    float cosUmbra;       // 44

    vec3 attenuation;     // 48
    float cosPenumbra;    // 60

    int  lightType;       // 64: 1=Point, 2=Spot
    int  shadowIndex;     // 68: -1=none, >=0=index
    float padding;        // 72
    float _vp_pad;        // 76 — explicit pad before mat4 (already 16B aligned at 80, but be explicit)

    // C++ static_assert (RHIShaderCommon.h:101) requires viewProjection at offset 128.
    // Slot 80..127 is 48 bytes of reserved space — unused in our path but kept for
    // byte-for-byte parity with C++ sizeof(LightParameters)=192. Explicit vec4 pads
    // prevent std140 from collapsing the gap.
    vec4 _vp_reserved_0;  // 80
    vec4 _vp_reserved_1;  // 96
    vec4 _vp_reserved_2;  // 112

    mat4 viewProjection;  // 128: spot light shadow matrix — total 128+64=192 bytes ✓
};
// Total: 192 bytes — matches C++ static_assert (RHIShaderCommon.h:101).

// ================================================================================================
// DirectionalLightParameters — 304 bytes (matches C++ RHIShaderCommon.h:75-83)
// ================================================================================================
struct DirectionalLightParameters {
    mat4 viewProjections[4];      // 0: 4 cascade matrices (4*64=256 bytes)
    vec4 splits;                  // 256: cascade split distances
    vec4 directionAndIntensity;   // 272: xyz=direction, w=intensity
    vec4 colorAndShadow;          // 288: rgb=color, a=shadow enabled
};
// Total: 304 bytes.

// ================================================================================================
// ForwardLightBuffer — 25808 bytes (matches C++ RHIShaderCommon.h:86-92)
// Layout: header(16) + 4*DirectionalLight(1216) + 128*Light(16384) + struct tail = 25808.
// Actually: 16 + 4*304 + 128*192 = 16 + 1216 + 24576 = 25808. ✓
//
// std140 in GLSL forces array stride to round up: mat4 inside struct uses 16B align,
// so sizeof(LightParameters) in GLSL std140 may differ from C++ 192 bytes.
// Workaround: declare as raw float[] buffer + index manually if validation complains.
// ================================================================================================
struct ForwardLightBuffer {
    uint directionalLightCount;   // 0
    uint punctualLightCount;      // 4
    uint _pad0;                   // 8
    uint _pad1;                   // 12

    DirectionalLightParameters directionalLights[4]; // 16
    LightParameters             lights[128];         // 16+1216=1232
};
// Total in C++: 16 + 4*304 + 128*192 = 25808 bytes.

// ================================================================================================
// MaterialUniforms / PBRMaterialParameters — auxiliary structures
// ================================================================================================
struct MaterialUniforms {
    vec4 color;
};

struct PBRMaterialParameters {
    vec4  baseColorFactor;     // 0
    vec3  emissiveFactor;      // 16
    float roughnessFactor;     // 28
    float metallicFactor;      // 32
    float normalScale;         // 36
    float occlusionStrength;   // 40
    float _pad;                // 44

    int hasBaseColorTexture;       // 48
    int hasNormalTexture;          // 52
    int hasMetallicRoughnessTexture; // 56
    int hasOcclusionTexture;       // 60
    int hasEmissiveTexture;        // 64
    int _pad2_0;                   // 68
    int _pad2_1;                   // 72
    int _pad2_2;                   // 76
};

#endif // RHIS_SHADER_TYPES_GLSL
