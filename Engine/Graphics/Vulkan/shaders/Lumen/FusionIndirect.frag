#version 450 core
// Vulkan Lumen FusionIndirect — GLSL port of DeferredLighting.metal::fragmentFusionIndirect.
// Entry point: fusion_indirect  (glslangValidator --source-entrypoint main -e fusion_indirect)
//
// Full-res pass: pre-combine GI sources + albedo + ssao -> indirect contribution.
// DDGI remains a half-res low-frequency input, while full-res SPGI/albedo are
// preserved at receiver edges instead of being destructively downsampled.

#extension GL_EXT_samplerless_texture_functions : enable
#ifdef GI_NO_ALL
#define GI_NO_DDGI
#define GI_NO_SPGI
#define GI_NO_SSGI
#endif
//
// Flat descriptor bindings (Metal has implicit samplers; Vulkan needs explicit):
//   binding 0 = SampledImage  ssgiColor
//   binding 1 = SampledImage  ddgiColor
//   binding 2 = SampledImage  spgiColor
//   binding 3 = SampledImage  albedoTex
//   binding 4 = SampledImage  ssaoTex
//   binding 5 = Sampler       defaultSampler  (Vulkan-only)

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D ssgiColor;
layout(set = 0, binding = 1) uniform texture2D ddgiColor;
layout(set = 0, binding = 2) uniform texture2D spgiColor;
layout(set = 0, binding = 3) uniform texture2D albedoTex;
layout(set = 0, binding = 4) uniform texture2D ssaoTex;
layout(set = 0, binding = 5) uniform sampler defaultSampler;

void fusion_indirect() {
    vec4 ssgi4   = texture(sampler2D(ssgiColor, defaultSampler), inUv);
    vec3 ddgi    = texture(sampler2D(ddgiColor, defaultSampler), inUv).rgb;
    vec4 spgi4   = texture(sampler2D(spgiColor, defaultSampler), inUv);
#ifdef GI_NO_DDGI
    ddgi = vec3(0.0);
#endif
#ifdef GI_NO_SPGI
    spgi4 = vec4(0.0);
#endif
    vec3 albedo  = texture(sampler2D(albedoTex, defaultSampler), inUv).rgb;
    float ssao   = texture(sampler2D(ssaoTex, defaultSampler), inUv).r;
    if (ssao <= 0.0) ssao = 1.0;
    // SSAO is a local contact-shadow approximation, not a validity mask for
    // probe lighting.  Let it shape DDGI without erasing the low-frequency
    // coloured bounce in the heavily occluded second-floor arcades.
    // 2026-09-06 UE 参考对齐：跟随 Metal 校准（54c9b8c2）——遮挡区 AO 压制
    // 加强，暗拱内部不再被 GI 填充抬亮。
    float ao = mix(0.05, 1.0, pow(ssao, 1.2));

    vec3 ssgi_irr  = ssgi4.rgb;
    float ssgi_hit  = ssgi4.a;

    // GI_ISOLATE A/B (M8/T8.7): compile-time variants that zero one GI source
    // each — identifies which path actually feeds a pixel region.
#ifdef GI_NO_SSGI
    ssgi_irr = vec3(0.0);
    ssgi_hit = 0.0;
#endif

    // SSGI confidence: alpha stores average hit distance in world units
    // (max_trace_distance = 30). Normalize against the trace cap, not a
    // hardcoded 2.0 (which zeroed out nearly all SSGI contribution since
    // typical hit distances are 5-20 units). Also clamp hit to [0, 30] to
    // guard against garbage values from the temporal/filter pipeline.
    float ssgi_hit_norm = clamp(ssgi_hit, 0.0, 30.0) / 30.0;
    float ssgi_conf = clamp(1.0 - ssgi_hit_norm * 0.7, 0.3, 1.0);

    // Both probe sources now carry irradiance in the same units. Confidence
    // controls replacement; selecting only brighter samples biases feedback.
    float spgi_w = clamp(spgi4.a, 0.0, 0.9);

    albedo = clamp(albedo, vec3(0.0), vec3(1.0));

    vec3 gi = mix(ddgi, spgi4.rgb, spgi_w)
            + ssgi_irr * ssgi_conf * mix(0.25, 0.15, spgi_w);


    // No constant ambient floor: indirect energy comes from the traced caches.

    // B3: indirect follows the direct chain's Lambert law (albedo/π·E).
    // B9: the full-stack `indirect *= ao` is removed — GTAO double-occluded
    // the near-field GI (probe visibility already attenuates it) while the
    // far-field floor above keeps its AO modulation (its only visibility cue).
    vec3 indirect = albedo * gi * 0.31830988;
    outColor = vec4(indirect, 1.0);
}
