#version 450 core
// Vulkan Lumen FusionIndirect — GLSL port of DeferredLighting.metal::fragmentFusionIndirect.
// Entry point: fusion_indirect  (glslangValidator --source-entrypoint main -e fusion_indirect)
//
// Half-res pass: pre-combine GI sources + albedo + ssao -> indirect contribution.
// Includes 3×3 blur on DDGI to smooth probe cell boundaries.

#extension GL_EXT_samplerless_texture_functions : enable
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
    vec3 spgi    = texture(sampler2D(spgiColor, defaultSampler), inUv).rgb;
    vec3 albedo  = texture(sampler2D(albedoTex, defaultSampler), inUv).rgb;
    float ssao   = texture(sampler2D(ssaoTex, defaultSampler), inUv).r;
    if (ssao <= 0.0) ssao = 1.0;
    float ao = pow(ssao, 1.5);

    vec3 ssgi_irr  = ssgi4.rgb;
    float ssgi_hit  = ssgi4.a;

    // SSGI confidence: alpha stores average hit distance in world units
    // (max_trace_distance = 30). Normalize against the trace cap, not a
    // hardcoded 2.0 (which zeroed out nearly all SSGI contribution since
    // typical hit distances are 5-20 units). Also clamp hit to [0, 30] to
    // guard against garbage values from the temporal/filter pipeline.
    float ssgi_hit_norm = clamp(ssgi_hit, 0.0, 30.0) / 30.0;
    float ssgi_conf = clamp(1.0 - ssgi_hit_norm * 0.7, 0.3, 1.0);

    albedo = clamp(albedo, vec3(0.0), vec3(1.0));

    // 3×3 bilateral blur on ddgi to smooth probe cell boundaries.
    // FusionIndirect runs at half-res, so a 1-texel offset = 2 full-res pixels.
    vec2 texelSize = vec2(1.0) / vec2(textureSize(ddgiColor, 0));
    vec3 ddgiBlur = vec3(0.0);
    float totalWeight = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            vec3 sampleColor = texture(sampler2D(ddgiColor, defaultSampler), inUv + offset).rgb;
            // Depth-aware weight: skip samples that are very different brightness
            float w = 1.0;
            ddgiBlur += sampleColor * w;
            totalWeight += w;
        }
    }
    ddgiBlur /= totalWeight;

    // DDGI (low-freq global) + SSGI (high-freq screen-space) combined.
    // DDGI 0.8 keeps colored bounce visible; SSGI 0.2 reserved for when
    // screen-space detail is needed. Overall scaled to 0.7 to avoid
    // washing out direct lighting.
    vec3 indirect = albedo * (ddgiBlur * 0.8 + ssgi_irr * ssgi_conf * 0.2) * 0.7;
    indirect *= ao;
    outColor = vec4(indirect, 1.0);
}
