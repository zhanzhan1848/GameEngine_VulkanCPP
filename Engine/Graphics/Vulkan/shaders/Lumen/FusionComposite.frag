#version 450 core
// Vulkan Lumen FusionComposite — GLSL port of DeferredLighting.metal::fragmentFusion.
// Entry point: fusion_composite  (glslangValidator --source-entrypoint main -e fusion_composite)
//
// Full-res pass: scene + pre-combined indirect + SSR + volume scatter -> HDR output.
//
// Flat descriptor bindings:
//   binding 0 = SampledImage  sceneColor
//   binding 1 = SampledImage  indirectColor
//   binding 2 = SampledImage  volumeScatter
//   binding 3 = SampledImage  ssrColor       (RGBA16F: RGB=reflection, A=strength)
//   binding 4 = SampledImage  ormTex         (AO.r, Roughness.g, Metallic.b)
//   binding 5 = Sampler       defaultSampler  (Vulkan-only)

#extension GL_EXT_samplerless_texture_functions : enable

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D sceneColor;
layout(set = 0, binding = 1) uniform texture2D indirectColor;
layout(set = 0, binding = 2) uniform texture2D volumeScatter;
layout(set = 0, binding = 3) uniform texture2D ssrColor;
layout(set = 0, binding = 4) uniform texture2D ormTex;
layout(set = 0, binding = 5) uniform sampler defaultSampler;

void fusion_composite() {
    vec3 scene    = texture(sampler2D(sceneColor,    defaultSampler), inUv).rgb;
    vec3 indirect = texture(sampler2D(indirectColor, defaultSampler), inUv).rgb;
    vec4 vol      = texture(sampler2D(volumeScatter, defaultSampler), inUv);
    vec4 ssr      = texture(sampler2D(ssrColor,      defaultSampler), inUv);

    // SSR reflection: ssr.a carries the accumulated strength mask (already
    // roughness-attenuated in the upsample pass). Apply a global scale factor
    // to keep reflections subtle — the upsample pass already multiplied by
    // reflection_strength (0.7), and the raw hit color can be bright HDR.
    // The 0.5 factor here brings the net contribution to a gentle overlay.
    float reflWeight = ssr.a * 0.25;
    vec3 reflection = ssr.rgb * reflWeight;

    // Beer-Lambert: attenuate scene by transmittance, add scattered light.
    // Output HDR linear — tonemap is handled by FinalBlit (Blit.frag) to avoid
    // double tonemap (which caused black patches in indirect-lit areas).
    vec3 result = (scene + indirect + reflection) * vol.a + vol.rgb;
    result = clamp(result, vec3(0.0), vec3(64.0));

    outColor = vec4(result, 1.0);
}
