#version 450 core
// Lightmap composite — 旁路改写 FusionIndirect 的间接漫反射信号。
// Docs/2026-10-19-lightmap-baking-and-rendering-architecture-plan.md
// Entry point: lightmap_composite
//
// 该 pass 位于 fusion 最终输出与后处理之间（独立消费级，2026-10-20 用户
// 定调：与 Lumen 动态 GI 解耦）：读取场景最终 HDR 颜色，对持有 lightmap
// 的像素整像素替换为烘焙完整光照（直射+间接，离线路径追踪产物），其余
// 像素原样透传。Lumen 侧 shader 零改动、零感知。
//
// Flat descriptor bindings（Vulkan 需显式 sampler；Metal M5 移植用隐式）：
//   binding 0 = SampledImage  indirectIn   （场景最终 HDR 颜色，底图）
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
    // 诊断档必须先于替换分支判断：否则 z>0.5 像素永远进不了诊断路径
    // （实测把档 3 变成死代码、绿色不可达的坑）。
    if (params.debugMode > 2.5) {
        // 诊断档 3：UV RT 直视——z>0.5（有 lightmap）绿色，否则红色。
        // 用于回答"UV pre-pass 到底写没写进可见像素"。
        result = uvMarker.z > 0.5 ? vec3(0.0, 1.0, 0.0) : vec3(0.6, 0.05, 0.05);
    } else if (params.debugMode > 1.5) {
        // 诊断档 2：纯色常量——只验证参数到达与消费链，不涉采样。
        result = vec3(1.0, 0.0, 1.0);
    } else if (params.debugMode > 0.5) {
        // 诊断档 1：屏幕 UV 直接采样测试页（上传/采样链路判别）。
        vec3 lm = texture(sampler2D(lightmapTex, defaultSampler), inUv).rgb;
        result = lm * params.indirectGain;
    } else if (uvMarker.z > 0.5) {
        // 纯模式：有 lightmap 的像素以烘焙页采样值替换间接漫反射。
        vec3 lm = texture(sampler2D(lightmapTex, defaultSampler), uvMarker.xy).rgb;
        result = lm * params.indirectGain;
    }
    outColor = vec4(result, 1.0);
}
