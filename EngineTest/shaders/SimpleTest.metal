#include <metal_stdlib>
using namespace metal;

// Absolute simplest test shader - just generates a colored triangle
struct SimpleVertexOut {
    float4 position [[position]];
    float4 color;
};

vertex SimpleVertexOut simple_test_vertex(uint vertexID [[vertex_id]])
{
    SimpleVertexOut out;

    // Generate a simple colored triangle directly in clip space
    // This bypasses all buffer data and matrix transformations
    switch(vertexID) {
        case 0: // Bottom-left - Red
            out.position = float4(-0.5, -0.5, 0.5, 1.0);
            out.color = float4(1.0, 0.0, 0.0, 1.0);
            break;
        case 1: // Bottom-right - Green
            out.position = float4(0.5, -0.5, 0.5, 1.0);
            out.color = float4(0.0, 1.0, 0.0, 1.0);
            break;
        case 2: // Top-center - Blue
            out.position = float4(0.0, 0.5, 0.5, 1.0);
            out.color = float4(0.0, 0.0, 1.0, 1.0);
            break;
        default:
            out.position = float4(0.0, 0.0, 0.5, 1.0);
            out.color = float4(1.0, 1.0, 1.0, 1.0);
            break;
    }

    return out;
}

fragment float4 simple_test_fragment(SimpleVertexOut in [[stage_in]])
{
    return in.color;
}