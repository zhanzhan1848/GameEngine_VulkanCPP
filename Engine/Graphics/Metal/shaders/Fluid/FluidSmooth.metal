#include <metal_stdlib>
using namespace metal;

struct FluidParams {
    float4 CameraPos;
    float4 ScreenParams;
    float4 InvViewProj[4];
    float4 ViewProj[4];
    float4 FluidParams1;
    float4 FluidParams2;
    float4 FluidParams3;
    float4 DepthParams;
};

// ============================================================================
// Fluid Smooth — Bilateral depth filter
// Edge-stopping: preserves depth discontinuities at fluid boundaries
// ============================================================================

kernel void fluid_smooth(
    texture2d<float, access::sample> depth_in    [[texture(0)]],
    texture2d<float, access::write>  depth_out    [[texture(1)]],
    constant FluidParams& params                  [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(params.ScreenParams.x) ||
        gid.y >= uint(params.ScreenParams.y)) return;

    constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);

    float centerDepth = depth_in.sample(s, float2(gid)).r;

    // No fluid at this pixel — pass through
    if (centerDepth <= 0.0) {
        depth_out.write(float4(0.0), gid);
        return;
    }

    float sigma = params.FluidParams2.x;
    if (sigma <= 0.0) {
        depth_out.write(float4(centerDepth), gid);
        return;
    }

    float sum = 0.0;
    float weightSum = 0.0;
    constexpr int kRadius = 2;

    for (int dy = -kRadius; dy <= kRadius; dy++) {
        for (int dx = -kRadius; dx <= kRadius; dx++) {
            int2 samplePos = int2(gid) + int2(dx, dy);
            if (samplePos.x < 0 || samplePos.y < 0) continue;

            float sampleDepth = depth_in.sample(s, float2(samplePos)).r;
            if (sampleDepth <= 0.0) continue;

            // Bilateral weights: spatial + depth
            float spatialR2 = float(dx * dx + dy * dy);
            float spatialW = exp(-spatialR2 / (2.0 * 1.5 * 1.5));

            float depthDiff = abs(sampleDepth - centerDepth) / max(centerDepth, 0.001);
            float depthW = exp(-depthDiff * depthDiff / (2.0 * sigma * sigma));

            float w = spatialW * depthW;
            sum += sampleDepth * w;
            weightSum += w;
        }
    }

    float result = (weightSum > 0.0) ? sum / weightSum : centerDepth;
    depth_out.write(float4(result), gid);
}
