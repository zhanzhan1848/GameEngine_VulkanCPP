#include <metal_stdlib>
using namespace metal;

vertex float4 line_vs(uint vid [[vertex_id]],
                      constant float4x4& view_proj [[buffer(2)]],
                      const device float3* vertices [[buffer(1)]]) {
    return view_proj * float4(vertices[vid], 1.0);
}

fragment float4 line_fs() {
    return float4(0.2, 1.0, 0.4, 1.0);
}
