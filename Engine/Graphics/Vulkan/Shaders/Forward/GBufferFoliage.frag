#version 450 core

// T4.6.5 part 10 — Vulkan port of Forward/GBufferFoliage.metal fragment stage.
// Entry point: main
//
// Foliage-specific logic vs GBuffer.frag:
//   - Instance-tinted albedo: outAlbedo = albedoSample * instanceBaseColor.
//   - Alpha-test gate: discard when albedo.a < instanceBaseColor.a * 0.5
//     (leaf cutout mask). Mirrors metal discard_fragment() at line 156.
//   - Instance-modulated ORM: roughness/metallic scaled by per-instance values.
//   - Velocity subtracts sceneData.jitter / previousJitter (GBuffer.frag does
//     not —Metal foliage line 188-189).
//
// Descriptor set layout (matches ForwardSceneRenderer::material_set_layout_):
//   set 1 binding 0 = SampledImage albedo
//   set 1 binding 1 = SampledImage normal
//   set 1 binding 2 = SampledImage ORM
//   set 1 binding 3 = Sampler
//
// Output attachments (matches GBuffer pipeline renderTargetFormats):
//   location 0 = BGRA8_UNorm  albedo
//   location 1 = RGBA16_Float normal
//   location 2 = BGRA8_UNorm  orm
//   location 3 = RG16_Float   velocity

#extension GL_EXT_samplerless_texture_functions : enable

// SceneData UBO is needed for jitter subtraction in the velocity calc.
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
    vec2 padding;
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
    // PBR sampling — instance-tinted albedo (Metal line 153).
    vec4 albedoSample = texture(sampler2D(albedoMap, defaultSampler), inUV);
    outAlbedo = albedoSample * inInstanceBaseColor;

    // Alpha-test gate for leaf cutouts (Metal line 156).
    if (outAlbedo.a < inInstanceBaseColor.a * 0.5) {
        discard;
    }

    // ORM sampling with instance modulation (Metal lines 161-170).
    vec4 ormSample = texture(sampler2D(ormMap, defaultSampler), inUV);
    if (length(ormSample.rgb) < 0.01) {
        outORM = vec4(1.0, inInstanceRoughness, inInstanceMetallic, 1.0);
    } else {
        outORM = vec4(ormSample.r,
                      ormSample.g * inInstanceRoughness,
                      ormSample.b * inInstanceMetallic,
                      1.0);
        outORM.r = max(outORM.r, 0.1);
    }

    // Normal mapping with fallback to interpolated vertex normal (Metal 173-183).
    vec3 normalSample = texture(sampler2D(normalMap, defaultSampler), inUV).rgb;
    if (length(normalSample) > 0.1) {
        vec3 normal = normalSample * 2.0 - 1.0;
        vec3 N = normalize(inWorldNormal);
        vec3 T = normalize(inWorldTangent);
        vec3 B = cross(N, T);
        mat3 TBN = mat3(T, B, N);
        outNormal = vec4(normalize(TBN * normal) * 0.5 + 0.5, 1.0);
    } else {
        outNormal = vec4(normalize(inWorldNormal) * 0.5 + 0.5, 1.0);
    }

    // Velocity with jitter subtraction (Metal 186-190). GBuffer.frag omits
    // the jitter term; foliage must keep it for stable TAA on swaying leaves.
    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    vec2 currentNDC_NoJitter  = currentNDC  - sceneData.jitter;
    vec2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;
}
