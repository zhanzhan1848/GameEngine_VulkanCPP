// ToneMapping.wgsl — ACES tone mapping with additive bloom.
// (AO/SSGI legacy compositing removed 2026-09-02 — see note in tonemap_fs.)

// Set to 1 to visualize velocity buffer (debug only). Set to 0 for normal rendering.
const DEBUG_VELOCITY: u32 = 0u;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var sceneTexture: texture_2d<f32>;
@group(0) @binding(1) var bloomTexture: texture_2d<f32>;
@group(0) @binding(2) var texSampler: sampler;
@group(0) @binding(3) var aoTexture: texture_2d<f32>;
@group(0) @binding(4) var ssgiTexture: texture_2d<f32>;
@group(0) @binding(5) var velocityTexture: texture_2d<f32>;

// Full-screen triangle vertex shader
@vertex
fn tonemap_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    let positions = array<vec4<f32>, 3>(
        vec4<f32>(-1.0, -1.0, 0.0, 1.0),
        vec4<f32>( 3.0, -1.0, 0.0, 1.0),
        vec4<f32>(-1.0,  3.0, 0.0, 1.0)
    );
    let uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var out: VertexOutput;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// 2026-09-02: 两平台基准观感是 fusion 线性 HDR 的直通显示（legacy 无 ACES）。
// tonemap_fs（ACES+gamma）保留为 PRIMAL_TONE_ACES=1 的可选 HDR 展示路径；
// 默认走 tonemap_fs_passthrough（仅 bloom 合成），与 Metal MSL 的
// tonemap_fs_passthrough 对齐——见 ToneMappingPass.cpp 的选择逻辑。
// 2026-09-06 UE 参考对齐：passthrough 同步 Metal 的 CAL_EXPOSURE=0.42
// 标定（ToneMapping.metal 同名系数）——此系数缺席使 Vulkan 全图比
// Metal 亮 ~2.4×，拱门暗部被抬亮（Sponza 同机位实测 mean 61 vs 31）。
const CAL_EXPOSURE: f32 = 0.26;

// B10.5 (2026-09-11)：tonemap_fs 是"上游线性 HDR"路径（Vulkan/Dawn 默认，
// 见 ToneMappingPass.cpp 的平台选择；passthrough 的 CAL_EXPOSURE 只服务
// Metal 的 LDR 上游，保持不动）。旧实现无曝光——HDR 直入 ACES，×50 显示
// 单位下向阳石墙 (~24) 必然饱和贴白。0.10 按 Sponza 同机位标定：
//   向阳墙 ~24 → ×0.10=2.4 → ACES 肩部 ~0.93 → gamma 后 ~0.97
//   天空 2.75   → 0.28 → ~0.41 → ~0.67
//   GI 阴影 0.05-0.3 → 12-58/255（暗部从贴地抬起）
// 偏亮/偏暗调此常数（有效域约 0.05-0.2），勿动 CAL_EXPOSURE（Metal 语义）。
const HDR_ACES_EXPOSURE: f32 = 0.10;

@fragment
fn tonemap_fs_passthrough(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let color = textureSample(sceneTexture, texSampler, uv).rgb;
    let bloom = textureSample(bloomTexture, texSampler, uv).rgb;
    return vec4<f32>((color + bloom) * CAL_EXPOSURE, 1.0);
}

// ACES Tone Mapping
fn ACESFilm(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

// Fragment shader
@fragment
fn tonemap_fs(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    // DEBUG: visualize velocity buffer
    if (DEBUG_VELOCITY == 1u) {
        let vel = textureSample(velocityTexture, texSampler, uv).xy;
        // Velocity is in NDC units (range ~[-1, 1]); amplify by 20x for visibility.
        // X→R channel (red = rightward motion), Y→G channel (green = upward motion).
        return vec4<f32>(abs(vel) * 20.0, 0.0, 1.0);
    }

    var color = textureSample(sceneTexture, texSampler, uv).rgb;

    // NaN/Inf guard (diagnostic + defensive). x != x catches NaN; abs > 3.4e38
    // catches ±Inf. If this guard fires (screen turns black where garbage
    // color was), the NaN originates upstream in sceneColor — SSGI/SSR/TAA
    // chain. If it doesn't fire and "pure color" persists, the source is the
    // GBuffer/DeferredLighting writing the same finite-but-wrong value.
    let nanMask = color != color;
    let infMask = abs(color) > vec3<f32>(3.4e38);
    if (any(nanMask) || any(infMask)) {
        color = vec3<f32>(0.0);
    }

    // 2026-09-02: AO/SSGI legacy compositing REMOVED (was: color *= ao.r;
    // color += ssgi * 0.4). FusionIndirect owns AO/GI composition now, and the
    // neutral dummies bound for bindings 3/4 were never initialized off-Dawn —
    // ao.r sampled 0 on Vulkan → whole frame multiplied to black (Vulkan 全链
    // 黑屏根因，二分实证：TONE_SOLID 红屏可达、真 draw 全黑)。Metal MSL
    // (ToneMapping.metal) 已经只有 scene+bloom+ACES —— 本修改对齐两平台。
    // If a non-fusion path ever needs AO/SSGI here again, the dummies must be
    // properly uploaded (white / black) on ALL platforms first.

    // Add bloom (simple additive)
    let bloom = textureSample(bloomTexture, texSampler, uv).rgb;
    var result = (color + bloom) * HDR_ACES_EXPOSURE;

    // Tone mapping
    result = ACESFilm(result);

    // Gamma correction
    result = pow(result, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(result, 1.0);
}
