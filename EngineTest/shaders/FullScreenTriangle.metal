#include <metal_stdlib>
using namespace metal;

#include "RHIShaderFunctions.metal"

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut fullscreen_triangle_vs(uint vertexID [[vertex_id]]) {
    VertexOut out;
    GetFullScreenTrianglePosUV(vertexID, out.position, out.uv);
    return out;
}
