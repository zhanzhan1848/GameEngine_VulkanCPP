// Lightmap composite — Metal 移植（2026-10-19 计划，M5）。
// 语义与 Vulkan shaders/Lightmap/LightmapComposite.frag 完全一致：
// fusion 间接信号的旁路改写级（见该文件头注释）。
//
// 绑定（与 LightmapModule 描述符集一致，Metal 纹理按 set 序）：
//   texture(0) = indirectIn（FusionIndirect 本帧输出）
//   texture(1) = lightmapTex（RGBA16F 烘焙页）
//   texture(2) = lightmapUV（UV pre-pass RT；z=0 无 lightmap）
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
};

fragment float4 lightmap_composite_fragment(
    LightmapVertexOut in [[stage_in]],
    texture2d<float> indirectIn [[texture(0)]],
    texture2d<float> lightmapTex [[texture(1)]],
    texture2d<float> lightmapUV [[texture(2)]],
    constant LightmapParams& params [[buffer(2)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none,
                        address::clamp_to_edge);

    float3 base = indirectIn.sample(s, in.uv).rgb;
    float3 result = base;

    const float4 uvMarker = lightmapUV.sample(s, in.uv);
    if (uvMarker.z > 0.5) {
        float3 lm = lightmapTex.sample(s, uvMarker.xy).rgb;
        result = lm * params.indirectGain;
    } else if (params.debugMode > 1.5) {
        result = float3(1.0, 0.0, 1.0);  // 诊断档 2：纯洋红
    } else if (params.debugMode > 0.5) {
        float3 lm = lightmapTex.sample(s, in.uv).rgb;  // 诊断档 1：采样测试页
        result = lm * params.indirectGain;
    }
    return float4(result, 1.0);
}
