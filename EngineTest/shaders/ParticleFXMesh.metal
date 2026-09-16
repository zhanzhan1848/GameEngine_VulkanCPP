// ParticleFXMesh.metal — PFX mesh 粒子渲染 shader（架构文档 §7-N2）。
//
// 顶点拉取（vertex pulling）+ 实例化：
//   buffer(0) 顶点位置表（float3 连续）
//   buffer(1) 顶点法线表（float3 连续）
//   buffer(2) 实例缓冲 —— render/Bridges.h 的 mesh_instance_data（48B，布局已锁）
//   buffer(3) 常量：view_projection + 调试色
// Metal 索引绘制下 vertex_id 语义 = 从索引缓冲取出的顶点索引（经典顶点拉取）。

#include <metal_stdlib>
using namespace metal;

// 与 render::mesh_instance_data（48B）逐字段对齐
struct PFXMeshInstance {
    float4 pos_and_pad;    // xyz = 世界位置, w = 实例色 R
    float4 rot;            // 四元数 [x,y,z,w]
    float4 scale_and_pad;  // xy = 缩放, zw = 实例色 GB
};

struct PFXMeshVP {
    float4x4 view_projection;
    float4   color;
};

struct PFXMeshVertexOut {
    float4 position [[position]];
    float3 normal;
    float4 color;
};

static inline float3 pfx_instance_tint(const PFXMeshInstance inst) {
    return float3(inst.pos_and_pad.w, inst.scale_and_pad.zw);
}

static inline float3 pfx_quat_rotate(float4 q, float3 v) {
    const float3 qv = q.xyz;
    const float3 t = 2.0 * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

vertex PFXMeshVertexOut pfx_mesh_vertex(
    device const float3*           positions [[buffer(0)]],
    device const float3*           normals   [[buffer(1)]],
    device const PFXMeshInstance*  instances [[buffer(2)]],
    constant PFXMeshVP&            vp        [[buffer(3)]],
    uint vid [[vertex_id]],
    uint iid [[instance_id]])
{
    const PFXMeshInstance inst = instances[iid];
    const float3 local = positions[vid] * max(inst.scale_and_pad.x, 1e-4f);
    const float3 world = inst.pos_and_pad.xyz + pfx_quat_rotate(inst.rot, local);
    const float3 world_n = pfx_quat_rotate(inst.rot, normals[vid]);

    PFXMeshVertexOut out;
    out.position = vp.view_projection * float4(world, 1.0);
    // 简单双灯 lambert（头灯 + 天光底光），调试观感足够辨识朝向
    const float3 n = normalize(world_n);
    const float3 l1 = normalize(float3(0.4, 1.0, 0.6));
    const float lambert = 0.35 + 0.65 * max(dot(n, l1), 0.0);
    out.color = vp.color * lambert * float4(pfx_instance_tint(inst), 1.0);
    out.normal = n;
    return out;
}

fragment float4 pfx_mesh_fragment(PFXMeshVertexOut in [[stage_in]])
{
    return float4(in.color.rgb, 1.0);
}
