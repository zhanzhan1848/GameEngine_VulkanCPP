// LightmapUV pre-pass — Metal 移植（2026-10-19 计划，M5）。
// 语义与 Vulkan shaders/Lightmap/LightmapUVPass.vert/.frag 一致：静态几何
// 按实体直绘（模块自管缓冲顶点拉取），输出逐像素 atlas UV + 有效位。
//
// 绑定（与 LightmapModule uv 描述符集一致，Metal buffer 按 set 序）：
//   buffer(0) = positions（float4 xyz+pad，世界空间）
//   buffer(1) = lightmapUVs（float2）
// push 常量（setVertexBytes @ buffer(2)，与 MetalCommandBuffer 约定一致）：
//   { float4x4 view_proj; uint position_offset; uint uv_offset;
//     float2 scale; float2 bias; }   // = C++ scale_bias[4]，见下面对齐注释

#include <metal_stdlib>
using namespace metal;

struct LightmapUvPush {
    float4x4 view_proj;
    uint position_offset;
    uint uv_offset;
    // 对应 C++ UvPushConstants::scale_bias[4]（float[4] @72）。必须拆成
    // 两个 float2（72/80）：合并 float4 会被 16B 对齐推到 80 与 C++ 错位
    // 8B——所有像素采到图集纹素 (0,0)（Vulkan 侧实测黑图根因）。
    float2 scale;  // scale_bias[0..1]
    float2 bias;   // scale_bias[2..3]
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
    out.atlasUV = lmUv * push.scale + push.bias;
    return out;
}

fragment float4 lightmap_uv_fragment(
    LightmapUvVertexOut in [[stage_in]])
{
    // xy = atlas UV（含实体 scale/bias），z = 有效位。
    // RT 清零（z=0）= 该像素无 lightmap——composite 据此走原路径。
    // w = 本 pass 重绘深度（[[position]].z，[0,1]）——2026-10-20 §9 取证
    // 插桩：composite 不读 w；DumpUvRT 用它与 GBuffer 深度差分定位偏移。
    return float4(in.atlasUV, 1.0, in.position.z);
}
