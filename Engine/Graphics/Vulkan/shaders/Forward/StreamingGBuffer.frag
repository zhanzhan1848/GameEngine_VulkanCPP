#version 450 core

// T4.6.5 part 15 — Vulkan port of Forward/StreamingGBuffer.metal fragment stage.
// Entry point: main
//
// Byte-identical to GBuffer.frag except for the velocity jitter de-offset
// (matches streamingFragmentMain's use of sceneData.jitter/previousJitter —
// GBuffer.frag currently omits that correction).
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

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outORM;
layout(location = 3) out vec2 outVelocity;

// Streaming defaults (StreamingGBuffer.metal:132-134 sets these to white/
// 0.5/0.0; they're per-instance in Metal but the streaming path leaves them
// at defaults since use_instances=0).
const vec4 instanceBaseColor = vec4(1.0, 1.0, 1.0, 1.0);
const float instanceRoughness = 0.5;
const float instanceMetallic  = 0.0;

void main() {
    vec4 albedoSample = texture(sampler2D(albedoMap, defaultSampler), inUV);
    outAlbedo = albedoSample * instanceBaseColor;

    vec4 ormSample = texture(sampler2D(ormMap, defaultSampler), inUV);
    if (length(ormSample.rgb) < 0.01) {
        outORM = vec4(1.0, instanceRoughness, instanceMetallic, 1.0);
    } else {
        outORM = vec4(ormSample.r,
                      ormSample.g * instanceRoughness,
                      ormSample.b * instanceMetallic,
                      1.0);
        outORM.r = max(outORM.r, 0.1);
    }

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

    // Velocity with jitter de-offset (matches Metal streamingFragmentMain:196-200).
    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    vec2 currentNDC_NoJitter = currentNDC - sceneData.jitter;
    vec2 previousNDC_NoJitter = previousNDC - sceneData.previousJitter;
    outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5;
}
