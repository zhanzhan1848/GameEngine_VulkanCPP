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
//   binding 2 = Sampler       defaultSampler
//   binding 3 = UniformBuffer params        （gain/debugMode）
//
// M0 骨架阶段（本文件当前形态）：仅 debugMode 全屏替换路径，用于验证
// pass 插针、纹理绑定、增益与 RG 纹理交接。M3 起接入 LightmapUV pre-pass
// 产出的逐像素 UV/index RT 与逐实体 scale/bias，替换条件随之改为
// "该像素 UV RT 有效"。

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D indirectIn;
layout(set = 0, binding = 1) uniform texture2D lightmapTex;
layout(set = 0, binding = 2) uniform sampler defaultSampler;

// 与 FusionIndirect.frag 的 GiGainUBO 同源增益（PRIMAL_GI_INDIRECT_GAIN，
// 默认 1.0 物理恒等）——lightmap 分支乘同一增益，保证 A/B 开关切换时
// 间接光标尺一致。debugMode：0=纯透传，1=全屏 debug 替换（M0 骨架）。
layout(set = 0, binding = 3) uniform LightmapParamsUBO {
    float indirectGain;
    float debugMode;
};

void lightmap_composite() {
    vec3 base = texture(sampler2D(indirectIn, defaultSampler), inUv).rgb;
    vec3 result = base;
    if (debugMode > 0.5) {
        // M0 debug 映射：屏幕 UV 直接当 lightmap UV（测试页为渐变图）。
        vec3 lm = texture(sampler2D(lightmapTex, defaultSampler), inUv).rgb;
        result = lm * indirectGain;
    }
    outColor = vec4(result, 1.0);
}
