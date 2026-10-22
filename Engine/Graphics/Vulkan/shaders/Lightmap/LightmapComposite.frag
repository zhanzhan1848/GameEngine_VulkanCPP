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
    // [§20 任务4] 采样柔化：bit0 = 亚纹素时域抖动（hash(pixel,frame) 偏移
    // ±0.5 texel → TAA 时域累积 = 对 lightmap 的时域超采样，静止视角等效
    // 柔化 26.7px texel 格）；bit1 = 4-tap 纹素足迹低通（±0.25 texel 四点
    // 均值，无时域依赖）。frameIndex 为抖动种子。
    float filterMode;
    float frameIndex;
    // [§21 问题2a] darkTint：无有效间接项的暗像素暖化强度（0=关）。
    // §20.1 定论：暗桶 80.5% texel E≈0 → base 裸露 deferred 暗部 fill
    // （IBL 环境镜面，B/R 0.968 中性偏蓝）；OFF 参考 0.804（暖材质调制）。
    // 该 tint 仅作用于 lightmap 合成路径（不动 Lumen/deferred 共用文件）。
    float darkTint;
} params;

const float kInvPi = 0.3183098861837907;

float Luma3(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// 整数 hash → [0,1)²（亚纹素抖动；同帧同像素确定、跨帧变化）。
vec2 Jitter2D(vec2 fragPx, float frameIdx) {
    uint h = uint(fragPx.x) * 1973u + uint(fragPx.y) * 9277u + uint(frameIdx) * 26699u;
    h = h * (h * h * 15631u + 2579u);
    return vec2(float(h & 65535u), float((h >> 16) & 65535u)) / 65535.0 - vec2(0.5);
}

// lightmap 采样（filterMode：bit0 抖动 / bit1 4-tap 足迹）。返回 rgb=加权
// E_ind、a=有效 tap 的 A 均值（全无效 tap 时 a<=0.25 → 调用方透传）。
// sampler2D 组合须在 point of use 构造（Vulkan 分离式规则），函数内自取。
vec4 SampleLightmap(vec2 atlasUv, vec2 fragPx) {
    const float fm = floor(params.filterMode + 0.5);
    const bool jitterOn = (fm == 1.0) || (fm == 3.0);
    const bool tap4On = (fm == 2.0) || (fm == 3.0);
    vec2 tx = 1.0 / vec2(textureSize(sampler2D(lightmapTex, defaultSampler), 0));
    vec2 jit = jitterOn ? Jitter2D(fragPx, params.frameIndex) * tx : vec2(0.0);
    vec2 base = atlasUv + jit;
    if (!tap4On) {
        return texture(sampler2D(lightmapTex, defaultSampler), base);
    }
    vec4 c0 = texture(sampler2D(lightmapTex, defaultSampler), base + vec2(-0.5, -0.5) * tx);
    vec4 c1 = texture(sampler2D(lightmapTex, defaultSampler), base + vec2( 0.5, -0.5) * tx);
    vec4 c2 = texture(sampler2D(lightmapTex, defaultSampler), base + vec2(-0.5,  0.5) * tx);
    vec4 c3 = texture(sampler2D(lightmapTex, defaultSampler), base + vec2( 0.5,  0.5) * tx);
    // 只平均 A>0.25 的 tap（空区不拖黑——同单 tap 语义）。
    vec4 sum = vec4(0.0);
    float n = 0.0;
    if (c0.a > 0.25) { sum += c0; n += 1.0; }
    if (c1.a > 0.25) { sum += c1; n += 1.0; }
    if (c2.a > 0.25) { sum += c2; n += 1.0; }
    if (c3.a > 0.25) { sum += c3; n += 1.0; }
    return n > 0.0 ? sum / n : vec4(0.0, 0.0, 0.0, 0.0);
}

void lightmap_composite() {
    vec3 base = texture(sampler2D(indirectIn, defaultSampler), inUv).rgb;
    vec3 result = base;
    bool hasIndirect = false;

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
        // §20 任务4：SampleLightmap = 亚纹素抖动/4-tap 足迹（filterMode）。
        vec4 lm = SampleLightmap(uvMarker.xy, gl_FragCoord.xy);
        if (lm.a > 0.25) {
            vec3 albedo = texture(sampler2D(gbufferAlbedo, defaultSampler), inUv).rgb;
            vec3 indirect = albedo * lm.rgb * (params.indirectGain * kInvPi);
            result = base + indirect;
            // [§21 问题2a] "有效间接"判据：叠加项已达可见量级（HDR 3e-3
            // ≈ 曝光后 >12/255；低于此的暖化不受间接豁免——A/B 实测 1e-4
            // 阈值使暗桶 70% 像素豁免、B/R 仅 1.101→1.066）。
            if (Luma3(indirect) > 3e-3) hasIndirect = true;
        }
        // A<=0.25（未栅格化空区/越界逃逸）：纯透传底图，绝不加黑。
    }
    // [§21 问题2a] PRIMAL_LIGHTMAP_DARKTINT（push: darkTint，默认 1.0，
    // 0=关）：无有效间接的暗像素对 base 做暖化。A/B 定参（§21）：暗桶
    // 无间接组 base B/R 实测 1.40-1.58（水面/织物/暗墙 IBL fill，比 §20.1
    // 的 0.968 更蓝），tint (1.0,0.94,0.74) 全强度下暗带 B/R 1.101→
    // ~0.86（OFF 参考 0.804，验收线 ≤0.90）。暗度权重 smoothstep
    // (0.004,0.05)：HDR >0.05（LDR ≈75+）w→0，中亮调零影响。
    if (params.darkTint > 0.0 && !hasIndirect && params.debugMode < 0.5) {
        float w = 1.0 - smoothstep(0.004, 0.05, Luma3(base));
        result = base * mix(vec3(1.0), vec3(1.0, 0.94, 0.74),
                            clamp(params.darkTint, 0.0, 1.0) * w);
    }
    outColor = vec4(result, 1.0);
}
