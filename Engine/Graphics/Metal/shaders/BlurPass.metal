#include <metal_stdlib>
using namespace metal;

struct BlurParams {
    uint textureWidth;
    uint textureHeight;
    int blurRadius;
    float sigma;
    uint arrayLayer; // 当前处理的 Layer
    uint direction;  // 0: Horizontal, 1: Vertical
};

// 高斯权重计算
float gaussianWeight(float x, float sigma) {
    return 0.39894228040143267794f / sigma * exp(-0.5f * x * x / (sigma * sigma));
}

// Compute Kernel for Texture2DArray
kernel void blurCS(
    texture2d_array<float, access::read> inputTexture [[texture(0)]],
    texture2d_array<float, access::write> outputTexture [[texture(1)]],
    constant BlurParams& params [[buffer(2)]],  // C++/layout binding 2 (matches SPIR-V/WGSL; buffer(0) here was never bound on Metal)
    uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.textureWidth || gid.y >= params.textureHeight) return;

    float4 result = float4(0.0);
    float totalWeight = 0.0;
    
    int radius = params.blurRadius;
    float sigma = params.sigma;
    
    // int2 coord = int2(gid.xy);
    // uint layer = params.arrayLayer;
    int2 coord = int2(gid.xy);
    uint layer = gid.z;
    
    if (params.direction == 0) { // Horizontal
        for (int i = -radius; i <= radius; ++i) {
            int x = clamp(coord.x + i, 0, (int)params.textureWidth - 1);
            float weight = gaussianWeight((float)i, sigma);
            result += inputTexture.read(uint2(x, coord.y), layer) * weight;
            totalWeight += weight;
        }
    } else { // Vertical
        for (int i = -radius; i <= radius; ++i) {
            int y = clamp(coord.y + i, 0, (int)params.textureHeight - 1);
            float weight = gaussianWeight((float)i, sigma);
            result += inputTexture.read(uint2(coord.x, y), layer) * weight;
            totalWeight += weight;
        }
    }

    outputTexture.write(result / totalWeight, uint2(gid.xy), layer);
}

// 2D variant — same gaussian blur for plain 2D textures (no array layer).
// Used when the caller passes non-array textures (e.g. VSM moments, whose
// MTLTextureType2D cannot bind to a texture2d_array parameter).
kernel void blurCS2D(
    texture2d<float, access::read> inputTexture [[texture(0)]],
    texture2d<float, access::write> outputTexture [[texture(1)]],
    constant BlurParams& params [[buffer(2)]],  // C++/layout binding 2 (matches SPIR-V/WGSL; buffer(0) here was never bound on Metal)
    uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.textureWidth || gid.y >= params.textureHeight) return;

    float4 result = float4(0.0);
    float totalWeight = 0.0;

    int radius = params.blurRadius;
    float sigma = params.sigma;

    int2 coord = int2(gid.xy);

    if (params.direction == 0) {
        for (int i = -radius; i <= radius; ++i) {
            int x = clamp(coord.x + i, 0, (int)params.textureWidth - 1);
            float weight = gaussianWeight((float)i, sigma);
            result += inputTexture.read(uint2(x, coord.y)) * weight;
            totalWeight += weight;
        }
    } else {
        for (int i = -radius; i <= radius; ++i) {
            int y = clamp(coord.y + i, 0, (int)params.textureHeight - 1);
            float weight = gaussianWeight((float)i, sigma);
            result += inputTexture.read(uint2(coord.x, y)) * weight;
            totalWeight += weight;
        }
    }

    outputTexture.write(result / totalWeight, uint2(gid.xy));
}
