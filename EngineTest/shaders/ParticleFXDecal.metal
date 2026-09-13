// ParticleFXDecal.metal — PFX decal 渲染器（架构文档 §7-N6，延迟贴花 v1）。
//
// 每实例 = 一个球形投影体积。全屏实例绘制（vid 0..5 × iid）：
//   片段：采样场景深度 → invViewProj 重建世界位置 → 距 decal 中心径向衰减着色。
//   buffer(0) 场景深度（depth2d）
//   buffer(1) decal 实例（48B PFXDecalInstance，与 render::decal_instance_data 对齐）
//   buffer(2) 常量：invViewProj + 屏幕尺寸
// 输出经 alpha 混合叠加到光照输出。

#include <metal_stdlib>
using namespace metal;

struct PFXDecalInstance {
    float4 center_radius;   // xyz = 世界中心, w = 半径
    float4 color;           // rgb = 贴花色, a = 强度
    float4 params;          // x = 软边比例
};

struct PFXDecalVP {
    float4x4 inv_view_proj;
    float4   screen_size;   // x = w, y = h
    float4   decal_color;
    float4   pad;
};

// 片段 stage_in 成员必须 float4 对齐（Metal 链接期要求）
struct PFXDecalVertexOut {
    float4 position [[position]];
    float4 uv_radius;     // xy = uv, z = radius
    float4 decal_center;
    float4 decal_color;
    float4 misc;          // x = softness
};

vertex PFXDecalVertexOut pfx_decal_vertex(
    device const PFXDecalInstance* decals [[buffer(1)]],
    uint vid [[vertex_id]],
    uint iid [[instance_id]])
{
    const PFXDecalInstance d = decals[iid];
    const float2 quadPos[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1),  float2(1, -1), float2(1, 1)
    };
    const float2 q = quadPos[vid];

    PFXDecalVertexOut out;
    out.position = float4(q, 0.0, 1.0);   // 全屏（深度只读，绘制顺序在光照后）
    out.uv_radius = float4(q * 0.5 + 0.5, max(d.center_radius.w, 1e-4f), 0.0);
    out.decal_center = d.center_radius;
    out.decal_color = d.color;
    out.misc = float4(d.params.x, 0.0, 0.0, 0.0);
    return out;
}

fragment float4 pfx_decal_fragment(
    PFXDecalVertexOut in [[stage_in]],
    depth2d<float> scene_depth [[texture(0)]],
    constant PFXDecalVP& vp [[buffer(2)]],
    sampler decal_sampler [[sampler(0)]])
{
    // 场景深度 → NDC z → 世界位置（invViewProj）
    const float scene_d = scene_depth.sample(decal_sampler, in.uv_radius.xy);
    if (scene_d >= 1.0) return float4(0, 0, 0, 0);   // 天空
    const float nx = in.uv_radius.x * 2.0 - 1.0;
    const float ny = in.uv_radius.y * 2.0 - 1.0;
    const float4 ndc = float4(nx, ny, scene_d, 1.0);
    float4 world = vp.inv_view_proj * ndc;
    world /= world.w;

    // 径向衰减（软边）
    const float3 d = world.xyz - in.decal_center.xyz;
    const float dist = length(d);
    const float t = saturate(dist / in.uv_radius.z);
    const float falloff = 1.0 - smoothstep(1.0 - in.misc.x, 1.0, t);
    if (falloff <= 0.0) return float4(0, 0, 0, 0);

    return float4(in.decal_color.rgb * falloff * in.decal_color.a, falloff);
}
