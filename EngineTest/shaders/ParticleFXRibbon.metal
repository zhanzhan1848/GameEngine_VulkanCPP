// ParticleFXRibbon.metal — PFX ribbon/trail 渲染 shader（架构文档 §7-N6）。
//
// 条带几何由 CPU 侧 render::ribbon_trail::build_strip 打包：
//   buffer(0) 顶点流（ribbon_vertex 32B：pos + uv，布局已锁）
//   buffer(1) 常量：view_projection + 颜色
// 片段：沿横向羽化（uv.x 边缘淡出）+ 头亮尾暗（uv.y 渐变）。

#include <metal_stdlib>
using namespace metal;

// 注意：Metal 的 float3 成员有 16B 对齐填充，用 float4 对精确表达 C++ 侧
// ribbon_vertex（32B）：pos[3]+pad → pos_uv.xyz/w；uv[2]+reserved[2] → uv_pad
struct RibbonPackedVertex {
    float4 pos_uv;    // xyz = 世界位置, w = uv.x
    float4 uv_pad;    // x = uv.y, yzw = reserved
};

static_assert(sizeof(RibbonPackedVertex) == 32, "ribbon vertex mismatch");

struct RibbonVP {
    float4x4 view_projection;
    float4   color;
};

struct RibbonVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};

vertex RibbonVertexOut pfx_ribbon_vertex(
    device const RibbonPackedVertex* vertices [[buffer(0)]],
    constant RibbonVP&               vp       [[buffer(1)]],
    uint vid [[vertex_id]])
{
    const RibbonPackedVertex v = vertices[vid];
    RibbonVertexOut out;
    out.position = vp.view_projection * float4(v.pos_uv.xyz, 1.0);
    out.uv = float2(v.pos_uv.w, v.uv_pad.x);
    out.color = vp.color;
    return out;
}

fragment float4 pfx_ribbon_fragment(RibbonVertexOut in [[stage_in]])
{
    // 横向羽化：边缘 |uv.x-0.5|*2 → 0..1，平方衰减
    const float edge = 1.0 - (in.uv.x - 0.5) * (in.uv.x - 0.5) * 4.0;
    const float taper = edge * edge;
    // 纵向：尾部（uv.y→0）渐隐
    const float along = 0.25 + 0.75 * in.uv.y;
    return float4(in.color.rgb * along, in.color.a * taper);
}
