// ParticleFXLight.metal — PFX light 渲染器（架构文档 §7-N6，Niagara Light renderer 对应物 v1）。
//
// 每个粒子渲染为 HDR 自发光 billboard（加色混合进 HDR 光照输出）——发光粒子
// 对最终画面的光贡献立即可见（tonemap 前的 HDR 加色）。
//   buffer(0) 光实例（32B）：float4 pos_intensity(xyz, w=HDR 强度)、float4 color_rgb_pad
//   buffer(1) 常量：view_projection
// 点精灵展开：vid 0..5 × instance_id（与 FluidSplat 同模式）。

#include <metal_stdlib>
using namespace metal;

struct PFXLightInstance {
    float4 pos_intensity;   // xyz = 世界位置, w = HDR 强度
    float4 color_rgb_pad;   // rgb = 颜色
};

struct PFXLightVP {
    float4x4 view_projection;
    float4   pad;
};

struct PFXLightVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
};

vertex PFXLightVertexOut pfx_light_vertex(
    device const PFXLightInstance* lights [[buffer(0)]],
    constant PFXLightVP&  vp [[buffer(1)]],
    uint vid [[vertex_id]],
    uint iid [[instance_id]])
{
    const PFXLightInstance L = lights[iid];
    const float3 world = L.pos_intensity.xyz;
    const float intensity = max(L.pos_intensity.w, 0.0);

    float4 centerClip = vp.view_projection * float4(world, 1.0);
    float clipW = max(centerClip.w, 0.001);

    float2 quadPos[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1),  float2(1, -1), float2(1, 1)
    };
    float2 q = quadPos[vid];

    // 屏幕半径随强度增大、随距离衰减（近大远小）
    float screenRadius = 0.02 * (1.0 + intensity * 0.05) / clipW;
    centerClip.x += q.x * screenRadius * clipW;
    centerClip.y += q.y * screenRadius * clipW;

    PFXLightVertexOut out;
    out.position = centerClip;
    out.color = float4(L.color_rgb_pad.rgb * intensity, 1.0);
    out.uv = q * 0.5 + 0.5;
    return out;
}

fragment float4 pfx_light_fragment(PFXLightVertexOut in [[stage_in]])
{
    // 径向衰减（软光斑）
    float2 c = (in.uv - 0.5) * 2.0;
    float r2 = dot(c, c);
    float falloff = saturate(1.0 - r2);
    falloff *= falloff;
    return float4(in.color.rgb * falloff, falloff);
}
