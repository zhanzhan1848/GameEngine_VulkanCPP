#version 450 core

// T4.6.5 part 12 — Vulkan port of Forward/GBufferWater.metal fragment stage.
// Entry point: main
//
// Water-specific logic (mirrors GBufferWater.metal:142-178):
//   1. Wave normal perturbation — sum of sine/cosine waves driven by
//      worldPos.xz and sceneData.time, scaled by 0.15 and added to +Y basis.
//   2. Fresnel — pow(1 - dot(V, N), 3) drives opacity (water gets more
//      transparent looking straight down).
//   3. Shallow/deep color mix — shallow=(0.1,0.6,0.7) teal, deep=(0.02,0.1,0.3)
//      navy; mix weight = fresnel (grazing angle → shallow).
//   4. ORM fixed at (1.0, 0.05, 0.0) — low roughness, no metallic.
//   5. Velocity computed with jitter subtraction (matches GBuffer.frag pattern
//      but adds the previousJitter subtraction that GBuffer.frag omits — the
//      Metal reference does both subtractions, so we mirror it here).
//
// Descriptor set layout (matches ForwardSceneRenderer::material_set_layout_):
//   set 1 binding 0 = SampledImage albedo   (unused — water synthesizes color)
//   set 1 binding 1 = SampledImage normal   (unused)
//   set 1 binding 2 = SampledImage ORM      (unused)
//   set 1 binding 3 = Sampler               (unused)
// Bindings still declared to preserve layout parity with sibling GBuffer
// shaders (ForwardSceneRenderer binds the same material set for all GBuffer
// variants). Samplerless extension kept for parity; no texture calls issued.
//
// Output attachments (matches GBuffer pipeline renderTargetFormats):
//   location 0 = BGRA8_UNorm  albedo
//   location 1 = RGBA16_Float normal
//   location 2 = BGRA8_UNorm  orm
//   location 3 = RG16_Float   velocity

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    float time;
    float _timePad;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
} sceneData;

layout(set = 1, binding = 0) uniform texture2D albedoMap;
layout(set = 1, binding = 1) uniform texture2D normalMap;
layout(set = 1, binding = 2) uniform texture2D ormMap;
layout(set = 1, binding = 3) uniform sampler    defaultSampler;

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
    // --- Wave normal perturbation (GBufferWater.metal:144-149) ---
    // Two crossed sinusoids on each axis, modulated by time. Sum is added to
    // +Y basis then normalized — produces a gently rippling surface normal.
    float t = sceneData.time;
    vec3 waveNormal;
    waveNormal.x = sin(inWorldPos.x * 2.0 + t * 1.5) * cos(inWorldPos.z * 1.8 + t * 1.2) * 0.15;
    waveNormal.y = 1.0;
    waveNormal.z = cos(inWorldPos.x * 1.6 + t * 1.8) * sin(inWorldPos.z * 2.2 + t * 1.0) * 0.15;
    waveNormal = normalize(waveNormal);

    // Perturb geometric normal with wave contribution
    vec3 N = normalize(inWorldNormal + waveNormal);

    // --- Fresnel (view-dependent opacity) ---
    vec3 viewDir = normalize(sceneData.viewPos.xyz - inWorldPos);
    float fresnel = pow(1.0 - max(dot(viewDir, N), 0.0), 3.0);

    // --- Shallow/deep water colors (GBufferWater.metal:159-162) ---
    vec3 shallowColor = vec3(0.1, 0.6, 0.7);
    vec3 deepColor    = vec3(0.02, 0.1, 0.3);
    vec3 waterColor = mix(shallowColor, deepColor, fresnel);
    // Metal reference: waterColor *= in.instanceBaseColor.rgb (line 162).
    waterColor *= inInstanceBaseColor.rgb;

    float opacity = mix(0.4, 0.95, fresnel);
    outAlbedo = vec4(waterColor, opacity);

    // --- Normal output ---
    outNormal = vec4(N * 0.5 + 0.5, 1.0);

    // --- ORM: low roughness, no metallic ---
    outORM = vec4(1.0, 0.05, 0.0, 1.0);

    // --- Velocity (jitter-subtracted, mirrors GBufferWater.metal:174-178) ---
    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    vec2 currentNDC_NoJitter = currentNDC - sceneData.jitter;
    vec2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;
}
