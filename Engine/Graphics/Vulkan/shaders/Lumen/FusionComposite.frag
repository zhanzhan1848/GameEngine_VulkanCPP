#version 450 core
// Vulkan Lumen FusionComposite — GLSL port of DeferredLighting.metal::fragmentFusion.
// Entry point: fusion_composite  (glslangValidator --source-entrypoint main -e fusion_composite)
//
// Full-res pass: scene + pre-combined indirect + reflections + volume scatter -> HDR output.
//
// Flat descriptor bindings:
//   binding 0 = SampledImage  sceneColor
//   binding 1 = SampledImage  indirectColor
//   binding 2 = SampledImage  volumeScatter
//   binding 3 = SampledImage  ssrColor       (RGBA16F: RGB=reflection, A=strength)
//   binding 4 = SampledImage  ormTex         (AO.r, Roughness.g, Metallic.b)
//   binding 5 = SampledImage  albedoTex      (metallic F0 source, T4.3)
//   binding 6 = SampledImage  ddgiSpecular   (half-res: RGB=SH(R) lobe radiance, A=NdotV, T4.3)
//   binding 7 = Sampler       defaultSampler  (Vulkan-only)
//   binding 8 = SampledImage  reflSpecular   (R4/R5: offscreen specular, RGB=fresnel-weighted
//                                             radiance, A=weight; black when v1)

#extension GL_EXT_samplerless_texture_functions : enable
#extension GL_GOOGLE_include_directive : require

// Shared shader library (axis refactor X1): the Karis EnvBRDF approximation
// lives in ShaderLib/EncodeLib.glsl.
#include "ShaderLib/EncodeLib.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D sceneColor;
layout(set = 0, binding = 1) uniform texture2D indirectColor;
layout(set = 0, binding = 2) uniform texture2D volumeScatter;
layout(set = 0, binding = 3) uniform texture2D ssrColor;
layout(set = 0, binding = 4) uniform texture2D ormTex;
layout(set = 0, binding = 5) uniform texture2D albedoTex;
layout(set = 0, binding = 6) uniform texture2D ddgiSpecular;
layout(set = 0, binding = 7) uniform sampler defaultSampler;
layout(set = 0, binding = 8) uniform texture2D reflSpecular;

// envBRDFApproxAB moved to ShaderLib/EncodeLib.glsl (axis refactor X1):
// returns the (scale, bias) pair, specular = F0 * AB.x + AB.y.

void fusion_composite() {
    vec3 scene    = texture(sampler2D(sceneColor,    defaultSampler), inUv).rgb;
    vec3 indirect = texture(sampler2D(indirectColor, defaultSampler), inUv).rgb;
    vec4 vol      = texture(sampler2D(volumeScatter, defaultSampler), inUv);
    vec4 ssr      = texture(sampler2D(ssrColor,      defaultSampler), inUv);
    vec4 orm      = texture(sampler2D(ormTex,        defaultSampler), inUv);
    vec3 albedo   = texture(sampler2D(albedoTex,     defaultSampler), inUv).rgb;
    vec4 ddgiSpec = texture(sampler2D(ddgiSpecular,  defaultSampler), inUv);
    vec4 reflOff  = texture(sampler2D(reflSpecular,  defaultSampler), inUv);

    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic  = orm.b;

    // T4.3 roughness relay: SSR owns the smooth segment (ssr.a is already
    // roughness-attenuated by the upsample pass); DDGI glossy irradiance
    // fills the rough end where screen-space tracing fades out.
    // (1 - roughness²) suppresses the double count with the diffuse indirect —
    // the widened lobe converges to E(N) which FusionIndirect already adds.
    float NoV = clamp(ddgiSpec.a, 0.05, 1.0);
    vec2 ab = envBRDFApproxAB(roughness, NoV);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 ddgiGlossy = ddgiSpec.rgb * (F0 * ab.x + ab.y);
    ddgiGlossy *= (1.0 - roughness * roughness) * 0.75;

    // [R3 2026-09-20] 粗糙度接力整定：ssr.rgb 已在 SSRComposite 内乘过
    // roughFade × strength(0.7)——旧 ×ssrW×0.25 是把强度二次衰减（v1 前
    // SSR 默认关、该路径从未被消费）。ssr.rgb 本身随 ssrW→0 收敛到 0，
    // 加法形态 + relay 兜底即为接力语义。
    // [R4/R5] 屏幕外 specular（WRC 探针 / SC 卡片 specular trace）：生产端
    // 已按 Fresnel 加权，仅在 SSR 未命中段 (1-ssrW) 接力；glossy relay 让出
    // 已被屏幕外 specular 覆盖的份额。v1（reflOff=black）行为退化回
    //   reflection = ssr.rgb + (1-ssrW)·ddgiGlossy。
    float ssrW = clamp(ssr.a, 0.0, 1.0);
    float offW = clamp(reflOff.a, 0.0, 1.0);
    vec3 reflection = ssr.rgb
                    + reflOff.rgb * offW * (1.0 - ssrW)
                    + ddgiGlossy * (1.0 - ssrW) * (1.0 - offW);

    // Beer-Lambert: attenuate scene by transmittance, add scattered light.
    // Output HDR linear — tonemap is handled by FinalBlit (Blit.frag) to avoid
    // double tonemap (which caused black patches in indirect-lit areas).
    // B10: (1-F)·(1-metallic) on the diffuse indirect — see Metal twin.
    vec3 F = F0 + (vec3(1.0) - F0) * pow(1.0 - NoV, 5.0);
    float kD = 1.0 - metallic;
    vec3 result = (scene + indirect * kD * (vec3(1.0) - F) + reflection) * vol.a + vol.rgb;
    result = clamp(result, vec3(0.0), vec3(64.0));

    outColor = vec4(result, 1.0);
}
