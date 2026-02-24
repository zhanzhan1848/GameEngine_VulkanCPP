#include <metal_stdlib>
using namespace metal;

struct SDFVertexOut {
    float4 position [[position]];
    float2 uv;
};

struct DebugUniforms {
    float4x4 viewProjection;
    float4x4 model;
    float4 params; // x = slice_depth, y = mode
    float4 padding; // Padding to match 160 bytes (C++ struct size)
};

// Quad for slice rendering (centered at origin, usually billboarded or fixed plane)
constant float3 quad_vertices[6] = {
    float3(-1, -1, 0), float3( 1, -1, 0), float3(-1,  1, 0),
    float3(-1,  1, 0), float3( 1, -1, 0), float3( 1,  1, 0)
};

vertex SDFVertexOut sdf_debug_vs(
    uint vertexID [[vertex_id]],
    constant DebugUniforms& uniforms [[buffer(1)]]
) {
    SDFVertexOut out;
    float3 pos = quad_vertices[vertexID];
    
    // Render the slice at the center of the world (or model origin)
    // Scale it up to be visible
    float4 worldPos = uniforms.model * float4(pos, 1.0); 
    out.position = uniforms.viewProjection * worldPos;
    out.uv = pos.xy * 0.5 + 0.5;
    return out;
}

fragment float4 sdf_slice_debug_fs(
    SDFVertexOut in [[stage_in]],
    texture3d<float> sdfTexture [[texture(0)]],
    constant DebugUniforms& uniforms [[buffer(1)]] // Using buffer 1 to match C++ Binding 1
) {
    // DEBUG: Force output red color to verify pipeline execution
    // return float4(1.0, 0.0, 0.0, 0.5);

    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    
    float slice_depth = uniforms.params.x;
    float3 uvw = float3(in.uv.x, in.uv.y, slice_depth);
    float dist = sdfTexture.sample(s, uvw).r;
    
    float3 color;
    float d = dist * 10.0; // Scale for visualization
    
    if (d < 0) {
        // Inside: Blue to Cyan
        color = mix(float3(0, 0, 1), float3(0, 1, 1), 1.0 + d);
        if (d < -1.0) color = float3(0, 0, 1);
    } else {
        // Outside: Red to Yellow
        color = mix(float3(1, 0, 0), float3(1, 1, 0), d);
        if (d > 1.0) color = float3(1, 1, 0);
    }
    
    // Isoline at 0
    if (abs(dist) < 0.01) color = float3(1, 1, 1);
    
    return float4(color, 0.8);
}
