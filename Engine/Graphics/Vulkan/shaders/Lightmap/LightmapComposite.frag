#version 450 core
// Lightmap composite — 间接辐照度叠加合成（v3 架构轮，2026-10-21）。
// Docs/2026-10-19-lightmap-baking-and-rendering-architecture-plan.md
// Entry point: lightmap_composite
//
// 该 pass 位于 deferred 直射输出（互斥态）或 fusion 输出（COEXIST 诊断态）
// 与后处理之间：底图 = 实时直射（真实材质纹理 + 实时阴影），lightmap 像素
// 叠加烘焙间接辐照度——烘焙页只存低频间接信号 E_ind（≥1 弹 + 天空），
// 合成公式：
//   final = base + GBufferAlbedo × lm.rgb / π × gain
// albedo 用 GBuffer 反照率（逐像素真实材质纹理）调制间接项：纹理细节/
// 直射锐利阴影来自实时链，间接补光是低频烘焙信号（16 tex/m 足够）。
// 页 A 通道 = 有效性（1 栅格化 / 0.5 dilation / 0 空）：A<阈值不加间接
// （退化为纯直射底图，绝不黑——§15.4 z=-8 整屏黑根因为 marker 采样落
// chart 内未栅格化空区）。
//
// Flat descriptor bindings（Vulkan 需显式 sampler；Metal M5 移植用隐式）：
//   binding 0 = SampledImage  indirectIn   （场景 HDR 颜色，底图）
//   binding 1 = SampledImage  lightmapTex  （RGBA16F 烘焙页，rgb=E_ind，a=有效性）
//   binding 2 = SampledImage  lightmapUV   （UV pre-pass RT；z=0 无 lightmap）
//   binding 3 = Sampler       defaultSampler
//   binding 4 = SampledImage  gbufferAlbedo（GBuffer 反照率，逐像素材质纹理）
// push constants（8B，像素阶段）:
//   float indirectGain  —— 间接增益（PRIMAL_LIGHTMAP_GAIN，v3 语义 = 仅
//                          作用于间接叠加项；默认 1.0 物理恒等）
//   float debugMode     —— 0=正常；1=全屏采样测试页；2=纯洋红；3/4=UV 直视
//
// 参数走 push 而非 UBO：全屏单绘制的小常量，且规避描述符池/在飞重写
// 的整类时序问题。

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D indirectIn;
layout(set = 0, binding = 1) uniform texture2D lightmapTex;
layout(set = 0, binding = 2) uniform texture2D lightmapUV;
layout(set = 0, binding = 3) uniform sampler defaultSampler;
layout(set = 0, binding = 4) uniform texture2D gbufferAlbedo;

layout(push_constant) uniform LightmapParams {
    float indirectGain;
    float debugMode;
} params;

const float kInvPi = 0.3183098861837907;

void lightmap_composite() {
    vec3 base = texture(sampler2D(indirectIn, defaultSampler), inUv).rgb;
    vec3 result = base;

    const vec4 uvMarker = texture(sampler2D(lightmapUV, defaultSampler), inUv);
    // 诊断档必须先于合成分支判断：否则 z>0.5 像素永远进不了诊断路径
    // （实测把档 3 变成死代码、绿色不可达的坑）。
    if (params.debugMode > 4.5) {
        // 互斥清零级（fusion 内）：输出全零 = Lumen GI 间接贡献关闭。
        result = vec3(0.0);
    } else if (params.debugMode > 3.5) {
        // 诊断档 4：UV RT 的 xy 直视（R=atlas u, G=atlas v, 无光像素黑）。
        result = vec3(uvMarker.xy, 0.0);
    } else if (params.debugMode > 2.5) {
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
        // 正常态：有 lightmap 的像素叠加间接。lm.rgb = 烘焙间接辐照度
        // E_ind（不含直射 NEE、不含纹素自身 albedo）；出射间接辐射亮度 =
        // albedo·E/π（Lambert BRDF）。albedo 取 GBuffer 逐像素反照率——
        // 间接项被真实材质纹理调制（v2 整像素替换丢纹理细节的修复）。
        vec4 lm = texture(sampler2D(lightmapTex, defaultSampler), uvMarker.xy);
        if (lm.a > 0.25) {
            vec3 albedo = texture(sampler2D(gbufferAlbedo, defaultSampler), inUv).rgb;
            result = base + albedo * lm.rgb * (params.indirectGain * kInvPi);
        }
        // A<=0.25（未栅格化空区/越界逃逸）：纯透传底图，绝不加黑。
    }
    outColor = vec4(result, 1.0);
}
