#version 450 core

// T4.6.5 part 12 — Vulkan port of Forward/GBufferUnlit.metal fragment stage.
// Entry point: main
//
// Unlit emissive path: writes base color directly to albedo with no lighting,
// no normal mapping, no ORM sampling. Sentinel values are written to the
// normal/orm attachments so downstream deferred lighting sees a no-op surface.
//
// Output attachments (matches GBuffer pipeline renderTargetFormats):
//   location 0 = BGRA8_UNorm  albedo   — flat base color (white default;
//                                         Metal version reads instanceBaseColor
//                                         from vertex stage, which the Vulkan
//                                         template doesn't plumb through yet)
//   location 1 = RGBA16_Float normal   — flat +Z sentinel (0.5, 0.5, 1.0)
//   location 2 = BGRA8_UNorm  orm      — AO=1, Roughness=1, Metallic=0
//   location 3 = RG16_Float   velocity — jitter-corrected motion vector
//
// Note: the Metal fragment reads sceneData.jitter/previousJitter to subtract
// TAA jitter from the velocity vector. We bind SceneData UBO (set 0 binding 1)
// to mirror that exactly.

#define SET_GLOBAL 0

layout(set = SET_GLOBAL, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    vec2 padding;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec3 inWorldTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in vec4 inCurrentPos;
layout(location = 5) in vec4 inPreviousPos;
layout(location = 6) in vec4 inInstanceBaseColor;
layout(location = 7) in float inInstanceRoughness;
layout(location = 8) in float inInstanceMetallic;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outORM;
layout(location = 3) out vec2 outVelocity;

void main() {
    // Unlit: output base color directly, no lighting (matches Metal fragmentMain
    // line `out.albedo = in.instanceBaseColor`). Vertex stage pipes the instance
    // material through; the non-instance branch defaults to white.
    outAlbedo = inInstanceBaseColor;

    // Flat normal facing +Z — encoded as (0.5, 0.5, 1.0) so downstream deferred
    // lighting (which does normal * 2.0 - 1.0) reconstructs (0, 0, +1).
    outNormal = vec4(0.5, 0.5, 1.0, 1.0);

    // ORM sentinel: AO=1, Roughness=1, Metallic=0 — fully rough, non-metallic,
    // full AO so deferred lighting attenuates nothing from this surface.
    outORM = vec4(1.0, 1.0, 0.0, 1.0);

    // Velocity with jitter subtraction (matches GBufferUnlit.metal:153).
    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    vec2 currentNDC_NoJitter = currentNDC - sceneData.jitter;
    vec2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;
}
