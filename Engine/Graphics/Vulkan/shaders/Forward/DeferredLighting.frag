#version 450 core

// T4.6.5 part 24.10 — minimal Vulkan port of Forward/DeferredLighting.metal
// fragmentLighting_v3. Full PBR port deferred — this is a smoke shader that
// declares all 9 bindings matching DeferredLightingModule's descriptor set
// layout (cpp:81-91) so pipeline creation succeeds and the deferred lighting
// pass writes non-trivial output. Real PBR math (ACES, IBL, shadows) is
// deferred to a follow-up port.
//
// Entry point: main
//
// Descriptor layout (matches DeferredLightingModule::Initialize cpp:81-91):
//   set 0 binding 0 = UniformBuffer ViewData (Vertex|Pixel)
//   set 0 binding 1 = UniformBuffer SceneData
//   set 0 binding 2 = SampledImage albedo
//   set 0 binding 3 = SampledImage normal
//   set 0 binding 4 = SampledImage orm
//   set 0 binding 5 = SampledImage depth
//   set 0 binding 6 = SampledImage shadowVisibility
//   set 0 binding 8 = Sampler
//   set 0 binding 9 = SampledImage fallback
//
// Engine convention: separate SampledImage + Sampler descriptors combined
// via sampler2D(tex, samp) idiom (matches Blit.frag).

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
};

layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
    vec2 jitter;
    vec2 previousJitter;
};

layout(set = 0, binding = 2) uniform texture2D albedoTex;
layout(set = 0, binding = 3) uniform texture2D normalTex;
layout(set = 0, binding = 4) uniform texture2D ormTex;
layout(set = 0, binding = 5) uniform texture2D depthTex;
layout(set = 0, binding = 6) uniform texture2D shadowVisTex;
layout(set = 0, binding = 8) uniform sampler defaultSampler;
layout(set = 0, binding = 9) uniform texture2D fallbackTex;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Sample GBuffer textures — proves descriptor binding layout works.
    vec4 albedo = texture(sampler2D(albedoTex, defaultSampler), inUv);
    vec3 N = texture(sampler2D(normalTex, defaultSampler), inUv).rgb;
    vec3 orm = texture(sampler2D(ormTex, defaultSampler), inUv).rgb;
    float depth = texture(sampler2D(depthTex, defaultSampler), inUv).r;
    float shadowVis = texture(sampler2D(shadowVisTex, defaultSampler), inUv).r;

    // Reconstruct world position from depth (Vulkan NDC: Z in [0,1], Y flipped vs Metal)
    vec2 ndcXY = vec2(inUv.x * 2.0 - 1.0, inUv.y * 2.0 - 1.0);
    vec4 ndc = vec4(ndcXY, depth, 1.0);
    vec4 worldPosH = invViewProjection * ndc;
    vec3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // Decode normal (stored as [0,1] → [-1,1])
    vec3 Nrm = normalize(N * 2.0 - 1.0);

    // Simple Lambertian + ambient
    vec3 L = normalize(lightPos.xyz - worldPos);
    float NdotL = max(dot(Nrm, L), 0.0);
    float ao = orm.r;
    vec3 diffuse = albedo.rgb * lightColor.rgb * NdotL * shadowVis;
    vec3 ambient = albedo.rgb * 0.15 * ao;
    outColor = vec4(diffuse + ambient, 1.0);
}
