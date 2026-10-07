#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full Screen Quad Vertex Shader
vertex VertexOut tonemap_vs(uint vertexID [[vertex_id]]) {
    float4 positions[3] = {
        float4(-1.0, -1.0, 0.0, 1.0),
        float4( 3.0, -1.0, 0.0, 1.0),
        float4(-1.0,  3.0, 0.0, 1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(2.0, 1.0),
        float2(0.0, -1.0)
    };

    VertexOut out;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// ACES Tone Mapping
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

// Metal 曝光标定系数（2026-09-05）。上游 GI/直射能量对齐完成前的整体
// 标定：同机位实测 Metal LDR 均值 131/255（贴白率 27.5%）vs Vulkan
// 40/255（0.7%）——用户判定 Metal 过曝、Vulkan 偏暗，取中点迭代。
// 上游 parity bisect 落地后本系数应回归 1.0。
// P0.3 (2026-09-15): 收编为 exposure_ev 单旋钮——标定值移入 ToneParams
// UBO（C++ 每帧上传 base × 2^EV，EV0 时与旧常数逐位同值，画面零变化）。
struct ToneParams {
    float exposure;   // 0
    uint  _pad0;      // 4
    uint  _pad1;      // 8
    uint  _pad2;      // 12
};

// Fragment Shader
// 注意（2026-09-11 B10.5）：两平台 ACES 路径的曝光语义有意分叉——Metal 的
// 上游是 LDR（fusion 已做过 exposure+ACES+gamma），继续用 0.26 基值；
// Vulkan/Dawn 的上游是线性 HDR，曝光在 WGSL tonemap_fs 的 0.10 基值。
// 不要把两边"对齐"——上游单位制不同。
fragment float4 tonemap_fs(VertexOut in [[stage_in]],
                           texture2d<float> sceneTexture [[texture(0)]],
                           texture2d<float> bloomTexture [[texture(1)]],
                           constant ToneParams& params [[buffer(6)]]) // Optional
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);

    float3 color = sceneTexture.sample(s, in.uv).rgb;

    // Add Bloom if available (simple addition)
    // Check if texture is bound by checking dimensions or assuming it is valid if passed
    // Metal doesn't allow checking for null texture easily in shader without flags
    // Assuming bloomTexture is bound (can be black 1x1 if disabled)
    float3 bloom = bloomTexture.sample(s, in.uv).rgb;
    color += bloom;

    // 曝光标定（P0.3: EV0 时 params.exposure 与旧 CAL_EXPOSURE 逐位同值）
    color *= params.exposure;

    // Tone Mapping
    color = ACESFilm(color);

    // Gamma Correction
    color = pow(color, float3(1.0/2.2));

    return float4(color, 1.0);
}

// Passthrough variant: bloom composite only, NO ACES/gamma.
// Metal 的上游（DeferredLighting.metal fragmentLighting / FusionComposite）
// 已经做过 exposure+ACES+gamma（LDR 输出）；全链再过一次 tonemap 是双重
// 转换，暗部被抬到近白（2026-09-02 同机位截图实证）。Vulkan 上游输出线性
// HDR，仍走 tonemap_fs。
fragment float4 tonemap_fs_passthrough(VertexOut in [[stage_in]],
                                       texture2d<float> sceneTexture [[texture(0)]],
                                       texture2d<float> bloomTexture [[texture(1)]],
                                       constant ToneParams& params [[buffer(6)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float3 color = sceneTexture.sample(s, in.uv).rgb;
    float3 bloom = bloomTexture.sample(s, in.uv).rgb;
    // 曝光标定（P0.3: EV0 时 params.exposure 与旧 CAL_EXPOSURE 逐位同值）——
    // passthrough 直出下线性值 ≥1.0 全部贴白，Metal GI 场均值 ~1.0 曾致
    // 27.5% 贴白率。
    color = (color + bloom) * params.exposure;
    return float4(color, 1.0);
}
