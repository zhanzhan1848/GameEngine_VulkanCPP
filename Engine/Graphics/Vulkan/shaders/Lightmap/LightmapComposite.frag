#version 450 core
// Lightmap composite — 旁路改写 FusionIndirect 的间接漫反射信号。
// Docs/2026-10-19-lightmap-baking-and-rendering-architecture-plan.md
// Entry point: lightmap_composite
//
// 该 pass 位于 FusionIndirect 与 FusionComposite 之间：读取 FusionIndirect
// 输出（post-albedo 出射辐亮度 Lo，量纲见 FusionCompositeModule.h 注释），
// 对持有 lightmap 的像素以烘焙页采样值替换，其余像素原样透传。FusionComposite
// 随后按原逻辑消费改写后的纹理——Lumen 侧 shader 零改动。
//
// Flat descriptor bindings（Vulkan 需显式 sampler；Metal M5 移植用隐式）：
//   binding 0 = SampledImage  indirectIn   （FusionIndirect 本帧输出）
//   binding 1 = SampledImage  lightmapTex  （RGBA16F 烘焙页）
//   binding 2 = SampledImage  lightmapUV   （UV pre-pass RT；z=0 无 lightmap）
//   binding 3 = Sampler       defaultSampler
// push constants（8B，像素阶段）:
//   float indirectGain  —— 与 FusionIndirect 同源增益（PRIMAL_GI_INDIRECT_GAIN）
//   float debugMode     —— 0=正常；1=全屏采样测试页；2=纯洋红（消费链诊断）
//
// 参数走 push 而非 UBO：全屏单绘制的小常量，且规避描述符池/在飞重写
// 的整类时序问题。

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D indirectIn;
layout(set = 0, binding = 1) uniform texture2D lightmapTex;
layout(set = 0, binding = 2) uniform texture2D lightmapUV;
layout(set = 0, binding = 3) uniform sampler defaultSampler;

layout(push_constant) uniform LightmapParams {
    float indirectGain;
    float debugMode;
} params;

void lightmap_composite() {
    vec3 base = texture(sampler2D(indirectIn, defaultSampler), inUv).rgb;
    vec3 result = base;

    const vec4 uvMarker = texture(sampler2D(lightmapUV, defaultSampler), inUv);
    if (uvMarker.z > 0.5) {
        vec3 lm = texture(sampler2D(lightmapTex, defaultSampler), uvMarker.xy).rgb;
        result = lm * params.indirectGain;
    } else if (params.debugMode > 1.5) {
        // 诊断档 2：纯色常量——只验证参数到达与消费链，不涉采样。
        result = vec3(1.0, 0.0, 1.0);
    } else if (params.debugMode > 0.5) {
        // 诊断档 1：屏幕 UV 直接采样测试页。
        vec3 lm = texture(sampler2D(lightmapTex, defaultSampler), inUv).rgb;
        result = lm * params.indirectGain;
    }
    outColor = vec4(result, 1.0);
}
