#include <metal_stdlib>
using namespace metal;

struct LineVertex {
    packed_float3 position;
    uint rgb;
};

struct LineOut {
    float4 position [[position]];
    float4 color;
};

vertex LineOut line_vs(uint vid [[vertex_id]],
                       constant float4x4& view_proj [[buffer(2)]],
                       const device LineVertex* vertices [[buffer(1)]]) {
    LineOut out;
    out.position = view_proj * float4(float3(vertices[vid].position), 1.0);
    uint rgb = vertices[vid].rgb;
    out.color = float4(float((rgb >> 16) & 0xff) / 255.0,
                       float((rgb >> 8) & 0xff) / 255.0,
                       float(rgb & 0xff) / 255.0,
                       1.0);
    return out;
}

fragment float4 line_fs(LineOut in [[stage_in]]) {
    return in.color;
}
