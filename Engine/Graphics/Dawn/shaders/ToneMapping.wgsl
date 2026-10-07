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

// P0.3 (2026-09-15): 曝光收编为 exposure_ev 单旋钮。C++ 侧每帧上传
// exposure = 路径标定基值 × 2^EV（EV 由 PRIMAL_TONE_EV 覆盖，默认 0）。
// EV0 时该值与下方两个标定常数逐位一致，画面零变化。
// P3.1 (2026-09-15): autoAdapt > 0.5（PRIMAL_TONE_ADAPT=1）时曝光由
// EyeAdaptation.comp 的 1×1 适应态（log2 avgLum）按 key/avgLum 缩放。
struct ToneParams {
    exposure: f32,   // 0
    autoAdapt: f32,  // 4
    key: f32,        // 8
    contrast: f32,   // 12  P3.3 0.5 枢轴对比（默认 1；P3 修正语义）
    saturation: f32, // 16  P3.3 饱和（默认 1）
    vignette: f32,   // 20  P3.3 暗角（默认 0）
    bloomIntensity: f32, // 24  P3: Bloom 合成增益（默认 1 = 原行为）
    _pad1: u32,      // 28
};
@group(0) @binding(6) var<uniform> params: ToneParams;
@group(0) @binding(7) var adaptState: texture_2d<f32>;

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
    // P0.3: CAL_EXPOSURE → params.exposure（EV0 时逐位同值，见 ToneParams 注释）
    return vec4<f32>((color + bloom) * params.exposure, 1.0);
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
    // P0.3: HDR_ACES_EXPOSURE → params.exposure（EV0 时逐位同值，见 ToneParams 注释）
    // P3.1: autoAdapt 开启时按 key/avgLum 缩放（适应态 = log2 平均亮度）
    var exposure = params.exposure;
    if (params.autoAdapt > 0.5) {
        let avgLum = exp2(textureLoad(adaptState, vec2<i32>(0, 0), 0).r);
        if (avgLum > 1e-6 && avgLum < 1e30) {
            exposure = exposure * params.key / avgLum;
        }
    }
    var result = (color + bloom * params.bloomIntensity) * exposure;

    // Tone mapping
    result = ACESFilm(result);

    // Gamma correction
    result = pow(result, vec3<f32>(1.0 / 2.2));

    // P3.3: color grading（默认中性：contrast=1 sat=1 vignette=0 画面零变化）
    // contrast：真 0.5 枢轴（显示域中点不动）。audit §4.9：旧实现
    // pow(result, contrast) 枢轴在 1.0、等效整体 gamma 偏移，与注释语义
    // 不符且调参人员无法按中点预期工作——修正为 (x-0.5)*c+0.5。
    if (params.contrast != 1.0) {
        result = clamp((result - vec3<f32>(0.5)) * params.contrast + vec3<f32>(0.5),
                       vec3<f32>(0.0), vec3<f32>(1.0));
    }
    // saturation：亮度混合
    if (params.saturation != 1.0) {
        let lum = dot(result, vec3<f32>(0.2126, 0.7152, 0.0722));
        result = mix(vec3<f32>(lum), result, vec3<f32>(params.saturation));
    }
    // vignette：从画面中心平滑衰减
    if (params.vignette > 0.0) {
        let d = distance(uv, vec2<f32>(0.5, 0.5));
        let falloff = 1.0 - params.vignette * smoothstep(0.35, 0.85, d);
        result = result * falloff;
    }

    return vec4<f32>(result, 1.0);
}
