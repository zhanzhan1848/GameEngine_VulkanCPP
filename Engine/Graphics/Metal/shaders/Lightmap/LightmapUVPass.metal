// LightmapUV pre-pass — Metal 移植（2026-10-19 计划，M5）。
// 语义与 Vulkan shaders/Lightmap/LightmapUVPass.vert/.frag 一致：静态几何
// 按实体直绘（模块自管缓冲顶点拉取），输出逐像素 atlas UV + 有效位。
//
// 绑定（与 LightmapModule uv 描述符集一致，Metal buffer 按 set 序）：
//   buffer(0) = positions（float4 xyz+pad，世界空间）
//   buffer(1) = lightmapUVs（float2）
// push 常量（setVertexBytes @ buffer(2)，与 MetalCommandBuffer 约定一致）：
//   { float4x4 view_proj; uint position_offset; uint uv_offset; float4 scale_bias; }

#include <metal_stdlib>
using namespace metal;

struct LightmapUvPush {
    float4x4 view_proj;
    uint position_offset;
    uint uv_offset;
    float4 scale_bias;
};

struct LightmapUvVertexOut {
    float4 position [[position]];
    float2 atlasUV;
};

vertex LightmapUvVertexOut lightmap_uv_vertex(
    uint vertexID [[vertex_id]],
    constant float4* positions [[buffer(0)]],
    constant float2* lightmapUvs [[buffer(1)]],
    constant LightmapUvPush& push [[buffer(2)]])
{
    LightmapUvVertexOut out;
    const float3 worldPos = positions[push.position_offset + vertexID].xyz;
    out.position = push.view_proj * float4(worldPos, 1.0);
    const float2 lmUv = lightmapUvs[push.uv_offset + vertexID];
    out.atlasUV = lmUv * push.scale_bias.xy + push.scale_bias.zw;
    return out;
}

fragment float4 lightmap_uv_fragment(
    LightmapUvVertexOut in [[stage_in]])
{
    // xy = atlas UV（含实体 scale/bias），z = 有效位，w = 1。
    // RT 清零（z=0）= 该像素无 lightmap——composite 据此走原路径。
    return float4(in.atlasUV, 1.0, 1.0);
}
