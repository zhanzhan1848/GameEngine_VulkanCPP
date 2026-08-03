#version 450 core

// T4.6.5 part 10 — Vulkan port of Forward/GBuffer.metal fragment stage.
// Entry point: main
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

void main() {
    vec4 albedoSample = texture(sampler2D(albedoMap, defaultSampler), inUV);
    outAlbedo = albedoSample;

    vec4 ormSample = texture(sampler2D(ormMap, defaultSampler), inUV);
    if (length(ormSample.rgb) < 0.01) {
        outORM = vec4(1.0, 0.5, 0.0, 1.0);
    } else {
        outORM = vec4(ormSample.r, ormSample.g * 0.5, ormSample.b * 0.0, 1.0);
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

    vec2 currentNDC = inCurrentPos.xy / inCurrentPos.w;
    vec2 previousNDC = inPreviousPos.xy / inPreviousPos.w;
    outVelocity = (currentNDC - previousNDC) * 0.5;
}
