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

// 2D variant — gaussian blur for plain 2D textures (no array layer).
// Used when the caller passes non-array textures (e.g. VSM moments, whose
// MTLTextureType2D cannot bind to a texture2d_array parameter).
//
// 2026-09-02 对齐：改为与 Vulkan blurCS（Dawn/shaders/BlurPass.wgsl，naga
// 编译成 BlurPass.spv）完全一致的 5-tap 线性采样高斯。此前 Metal 是
// σ=params.sigma 的真高斯循环（r=8/σ=1.0，中心权重 ~0.6，明显更锐），
// Vulkan 是 5-tap（0.227/0.316/0.070，σ≈1.14）——同一 VSM moments 链在
// 两平台 blur 出不同的方差分布，伪影1（楔形亮斑）/伪影2（阴影楼梯）/
// 直射阳光覆盖差异均源于此。weights 与 WGSL 逐位一致。
kernel void blurCS2D(
    texture2d<float, access::read> inputTexture [[texture(0)]],
    texture2d<float, access::write> outputTexture [[texture(1)]],
    constant BlurParams& params [[buffer(2)]],  // C++/layout binding 2 (matches SPIR-V/WGSL; buffer(0) here was never bound on Metal)
    uint3 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.textureWidth || gid.y >= params.textureHeight) return;

    int2 coord = int2(gid.xy);

    // 5-tap gaussian (linear-sampling variant), identical to BlurPass.wgsl:
    //   center 0.2270270270, ±1 0.3162162162, ±2 0.070270270
    // WGSL textureLoad 越界返回 0（spec 鲁棒性）——此处 lambda 等价实现。
    auto load0 = [&](int2 c) -> float4 {
        if (c.x < 0 || c.y < 0 || c.x >= (int)params.textureWidth || c.y >= (int)params.textureHeight)
            return float4(0.0);
        return inputTexture.read(uint2(c));
    };
    int2 offset = (params.direction == 0) ? int2(1, 0) : int2(0, 1);
    float4 result = inputTexture.read(uint2(coord)) * 0.2270270270;
    result += load0(coord + offset) * 0.3162162162;
    result += load0(coord - offset) * 0.3162162162;
    result += load0(coord + 2 * offset) * 0.070270270;
    result += load0(coord - 2 * offset) * 0.070270270;

    outputTexture.write(result, uint2(gid.xy));
}
