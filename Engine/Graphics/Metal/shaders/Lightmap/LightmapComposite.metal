// Lightmap composite — Metal 移植（2026-10-19 计划，M5）。
// 语义与 Vulkan shaders/Lightmap/LightmapComposite.frag 完全一致（v3 间接
// 辐照度叠加合成，2026-10-21 架构轮；见该文件头）：底图 = 实时直射，
// lightmap 像素叠加 GBufferAlbedo × lm.rgb / π × gain；页 A 通道有效性
// （1 栅格化 / 0.5 dilation / 0 空），A<阈值不加间接（纯透传底图）。
//
// 绑定（与 LightmapModule 描述符集一致；Metal 槽位 = binding 序号直映
// ——MetalCommandBuffer::BindDescriptorSets "slot = binding.binding"，
// sampler 为隐式 constexpr 不占纹理槽）：
//   texture(0) = indirectIn（场景 HDR 颜色，底图）
//   texture(1) = lightmapTex（RGBA16F 烘焙页，rgb=E_ind，a=有效性）
//   texture(2) = lightmapUV（UV pre-pass RT；z=0 无 lightmap）
//   texture(4) = gbufferAlbedo（GBuffer 反照率，逐像素材质纹理；binding 4）
// push 常量（MetalCommandBuffer::PushConstants → setFragmentBytes @ buffer(2)）：
//   { float indirectGain; float debugMode; }

#include <metal_stdlib>
using namespace metal;

// 与 DeferredLighting.metal::vertexMain 的输出同构（全屏三角形 VS 共用）。
struct LightmapVertexOut {
    float4 position [[position]];
    float2 uv;
};

struct LightmapParams {
    float indirectGain;
    float debugMode;
    // [§20 任务4] 与 Vulkan 版同步：bit0 亚纹素时域抖动 / bit1 4-tap 足迹。
    float filterMode;
    float frameIndex;
};

const constant float kInvPi = 0.3183098861837907;

float2 Jitter2D(float2 fragPx, float frameIdx) {
    uint h = uint(fragPx.x) * 1973u + uint(fragPx.y) * 9277u + uint(frameIdx) * 26699u;
    h = h * (h * h * 15631u + 2579u);
    return float2(float(h & 65535u), float((h >> 16) & 65535u)) / 65535.0 - 0.5;
}

float4 SampleLightmap(texture2d<float> lm, sampler s, float2 atlasUv, float2 fragPx,
                      float filterMode, float frameIdx) {
    const float fm = floor(filterMode + 0.5);
    const bool jitterOn = (fm == 1.0) || (fm == 3.0);
    const bool tap4On = (fm == 2.0) || (fm == 3.0);
    const float2 tx = 1.0 / float2(lm.get_width(), lm.get_height());
    const float2 jit = jitterOn ? Jitter2D(fragPx, frameIdx) * tx : float2(0.0);
    const float2 base = atlasUv + jit;
    if (!tap4On) return lm.sample(s, base);
    float4 c0 = lm.sample(s, base + float2(-0.5, -0.5) * tx);
    float4 c1 = lm.sample(s, base + float2( 0.5, -0.5) * tx);
    float4 c2 = lm.sample(s, base + float2(-0.5,  0.5) * tx);
    float4 c3 = lm.sample(s, base + float2( 0.5,  0.5) * tx);
    float4 sum = float4(0.0);
    float n = 0.0;
    if (c0.a > 0.25) { sum += c0; n += 1.0; }
    if (c1.a > 0.25) { sum += c1; n += 1.0; }
    if (c2.a > 0.25) { sum += c2; n += 1.0; }
    if (c3.a > 0.25) { sum += c3; n += 1.0; }
    return n > 0.0 ? sum / n : float4(0.0);
}

fragment float4 lightmap_composite_fragment(
    LightmapVertexOut in [[stage_in]],
    texture2d<float> indirectIn [[texture(0)]],
    texture2d<float> lightmapTex [[texture(1)]],
    texture2d<float> lightmapUV [[texture(2)]],
    texture2d<float> gbufferAlbedo [[texture(4)]],
    constant LightmapParams& params [[buffer(2)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none,
                        address::clamp_to_edge);

    float3 base = indirectIn.sample(s, in.uv).rgb;
    float3 result = base;

    const float4 uvMarker = lightmapUV.sample(s, in.uv);
    // 诊断档先于合成分支判断（与 Vulkan 版同坑：z>0.5 会吞掉诊断路径）。
    if (params.debugMode > 4.5) {
        // 互斥清零级（fusion 内）：输出全零 = Lumen GI 间接贡献关闭。
        result = float3(0.0);
    } else if (params.debugMode > 3.5) {
        // 诊断档 4：UV RT 的 xy 直视。
        result = float3(uvMarker.xy, 0.0);
    } else if (params.debugMode > 2.5) {
        // 诊断档 3：UV RT 直视——绿=有 lightmap 像素，红=无。
        result = uvMarker.z > 0.5 ? float3(0.0, 1.0, 0.0) : float3(0.6, 0.05, 0.05);
    } else if (params.debugMode > 1.5) {
        result = float3(1.0, 0.0, 1.0);  // 诊断档 2：纯洋红
    } else if (params.debugMode > 0.5) {
        float3 lm = lightmapTex.sample(s, in.uv).rgb;  // 诊断档 1：采样测试页
        result = lm * params.indirectGain;
    } else if (uvMarker.z > 0.5) {
        // 正常态：间接辐照度叠加（lm.rgb = E_ind，出射间接 = albedo·E/π）。
        // §20 任务4：SampleLightmap = 亚纹素抖动/4-tap 足迹（filterMode）。
        float4 lm = SampleLightmap(lightmapTex, s, uvMarker.xy, in.position.xy,
                                   params.filterMode, params.frameIndex);
        if (lm.a > 0.25) {
            float3 albedo = gbufferAlbedo.sample(s, in.uv).rgb;
            result = base + albedo * lm.rgb * (params.indirectGain * kInvPi);
        }
        // A<=0.25：纯透传底图，绝不加黑。
    }
    return float4(result, 1.0);
}
