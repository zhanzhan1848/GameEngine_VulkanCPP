#version 450 core
// Vulkan Lumen FusionIndirect — GLSL port of DeferredLighting.metal::fragmentFusionIndirect.
// Entry point: fusion_indirect  (glslangValidator --source-entrypoint main -e fusion_indirect)
//
// Full-res pass: pre-combine GI sources + albedo + ssao -> indirect contribution.
// DDGI remains a half-res low-frequency input, while full-res SPGI/albedo are
// preserved at receiver edges instead of being destructively downsampled.

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
    float ao = mix(0.4, 1.0, pow(ssao, 1.5));

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

    // SPGI weight (T2.3): gather output alpha = probe confidence. The dummy
    // 1x1 black texture bound when SPGI is disabled has alpha 0, and the
    // formula below then degrades to the legacy DDGI+SSGI blend.
    // Capped at 0.9 so the world-probe layer always keeps a floor, AND scaled
    // by the result luminance: in enclosed interiors every SPGI ray hits
    // geometry (hitRatio ≈ 1 → high confidence) but the geometry is unlit, so
    // SPGI is confidently black — exactly where the accumulated world-probe
    // ambient must take over instead of being suppressed to 10%.
    float spgiLum = dot(spgi4.rgb, vec3(0.2126, 0.7152, 0.0722));
    // T8.7: SPGI may only suppress the DDGI/ambient layers when it carries
    // REAL light. The old 0.002 lower bound let "confidently near-black"
    // arcade probes through (hitRatio≈1 on unlit corridor geometry), which
    // halved the sky-leak ambient and net-darkened the region by ~1.9/255
    // (measured, GI_ISOLATE=spgi A/B). Meaningful indirect in this scene's
    // linear scale is ≥0.02; full suppression rights at ≥0.15.
    float radianceValidity = smoothstep(0.02, 0.15, spgiLum);
    float spgi_w = clamp(spgi4.a * 2.0, 0.0, 0.9) * radianceValidity;

    albedo = clamp(albedo, vec3(0.0), vec3(1.0));

    // DDGI is the stable low-frequency field. SPGI may replace it only where
    // the screen-probe estimate carries at least as much energy; confidence
    // alone must never turn the DDGI floor down. The old cross-fade reduced
    // DDGI to 10% at high confidence and created dark troughs precisely beside
    // bright direct-light samples when a sparse SPGI lobe underestimated them.
    vec3 ddgiBase = ddgi * 0.7;
    vec3 spgiTarget = spgi4.rgb * 1.25;
    // T8.8: replacement needs a real brightness ADVANTAGE. At parity the
    // componentwise max() still swapped chroma (SPGI's tint over DDGI's),
    // which measured as a net darkening in the left-2F arcades vs the
    // no-SPGI ceiling (GI_ISOLATE=spgi A/B). Gate the merge on the luminance
    // ratio: ≤ +10% keeps pure ddgiBase, ≥ +35% takes the full max(); the ramp
    // avoids iso-luminance contouring on the low-frequency field. Confidence
    // stays a multiplier — bright-but-sparse SPGI must not hard-replace.
    float ddgiLum  = dot(ddgiBase,   vec3(0.2126, 0.7152, 0.0722));
    float spgiLumT = dot(spgiTarget, vec3(0.2126, 0.7152, 0.0722));
    float replaceW = smoothstep(1.10, 1.35, spgiLumT / max(ddgiLum, 1e-5));
    vec3 gi = mix(ddgiBase, max(ddgiBase, spgiTarget), spgi_w * replaceW)
            + ssgi_irr * ssgi_conf * mix(0.25, 0.15, spgi_w * replaceW);

    // Sky-leak ambient (Lumen far-field): deep interiors only reach via 2nd+
    // bounces whose geometric gain (~0.3^k) leaves them physically near-black;
    // every real building leaks sky bounce. Small hemispheric floor, albedo-
    // tinted, scaled down where SPGI already sees real light.
    // T8.7: the sky-leak floor is a FAR-FIELD path — additive with the
    // near-field SPGI estimate, and the max(ddgiBase, spgiTarget) replacement
    // already handles near-field redundancy. Measured (GI_ISOLATE=spgi):
    // suppressing the floor by SPGI confidence was a pure net-darkening
    // (-1.9/255 in the left-2F band, SPGI's real light there ≈ ddgiBase).
    // T8.9: source decomposition in the left-2F band (GI_ISOLATE=all/spgi/ddgi
    // steady-state) put the whole zone at direct+floor ≈ 28.6/255 with DDGI
    // delivering < 1/255 — probe/screen GI physically starves behind the thick
    // arcade walls until card coverage reaches the interior (separate task).
    // The floor IS the far-field representation here, so it must stand in for
    // the missing 2nd+ bounces off the sunlit hall, not just a whisper of sky:
    // ~1.7x up, warmed toward stone bounce colour. Direct-lit regions barely
    // register it next to their own term; shadow interiors carry the lift.
    // T8.10c: occlusion-modulated far field. The flat constant (T8.9) was a
    // crutch for a broken transport; with real probe light flowing it reads as
    // uniform wash in shadow pockets beside lit zones (user report: surfaces
    // that should be darker than the outer colonnade stay bright). SSAO keys
    // the floor down to ~half in crevices while open bounce areas keep it.
    gi += vec3(0.17, 0.18, 0.21) * mix(0.45, 1.0, pow(ssao, 1.1));

    vec3 indirect = albedo * gi;
    indirect *= ao;
    outColor = vec4(indirect, 1.0);
}
