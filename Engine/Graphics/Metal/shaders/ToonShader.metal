#include <metal_stdlib>
using namespace metal;

struct ToonConstants {
    float edgeThreshold;
    float colorLevels;
};

// Sobel Edge Detection
float detectEdge(texture2d<float, access::read> depthTex, texture2d<float, access::read> normalTex, uint2 coords) {
    // Simple depth-based edge detection
    // In production, should combine depth and normal discontinuities
    float d = depthTex.read(coords).r;
    
    float d_left = depthTex.read(coords + uint2(-1, 0)).r;
    float d_right = depthTex.read(coords + uint2(1, 0)).r;
    float d_up = depthTex.read(coords + uint2(0, -1)).r;
    float d_down = depthTex.read(coords + uint2(0, 1)).r;
    
    float edge = abs(d - d_left) + abs(d - d_right) + abs(d - d_up) + abs(d - d_down);
    return edge;
}

kernel void toon_main(
    texture2d<float, access::read> inColor [[texture(0)]],
    texture2d<float, access::read> inDepth [[texture(1)]],
    texture2d<float, access::read> inNormal [[texture(2)]],
    texture2d<float, access::write> outColor [[texture(3)]],
    constant ToonConstants& constants [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= outColor.get_width() || gid.y >= outColor.get_height()) {
        return;
    }

    float4 color = inColor.read(gid);
    
    // 1. Color Quantization
    float3 quantized = floor(color.rgb * constants.colorLevels) / constants.colorLevels;
    
    // 2. Edge Detection
    float edge = detectEdge(inDepth, inNormal, gid);
    float edgeFactor = edge > constants.edgeThreshold ? 0.0 : 1.0;
    
    outColor.write(float4(quantized * edgeFactor, color.a), gid);
}
