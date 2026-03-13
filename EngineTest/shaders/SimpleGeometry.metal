#include <metal_stdlib>
using namespace metal;

// Simple Geometry Shader - for testing basic rendering
struct VertexOut {
    float4 position [[position]];
    float3 color;
};

// Simple vertex shader that creates a colorful triangle
vertex VertexOut simple_vertex_shader(
    uint vid [[vertex_id]])
{
    VertexOut out;

    // Create a simple triangle in clip space
    switch(vid) {
        case 0:
            out.position = float4(-0.5, -0.5, 0.5, 1.0); // Bottom left
            out.color = float3(1.0, 0.0, 0.0); // Red
            break;
        case 1:
            out.position = float4(0.5, -0.5, 0.5, 1.0);  // Bottom right
            out.color = float3(0.0, 1.0, 0.0); // Green
            break;
        case 2:
            out.position = float4(0.0, 0.5, 0.5, 1.0);   // Top center
            out.color = float3(0.0, 0.0, 1.0); // Blue
            break;
        default:
            out.position = float4(0.0, 0.0, 0.5, 1.0);
            out.color = float3(1.0, 1.0, 1.0);
            break;
    }

    return out;
}

// Simple fragment shader
fragment float4 simple_fragment_shader(
    VertexOut in [[stage_in]])
{
    return float4(in.color, 1.0);
}