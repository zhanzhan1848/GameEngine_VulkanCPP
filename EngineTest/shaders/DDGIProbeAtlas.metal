// Test: write constant to atlas (bypass buffer reads)
#include <metal_stdlib>
using namespace metal;

struct AtlasParams {
    uint3  probeCounts;
    uint   probeCountTotal;
};

kernel void ddgi_prefilter_atlas(
    uint gid [[thread_position_in_grid]],
    constant AtlasParams& params [[buffer(0)]],
    device const float3* irradianceBuffer [[buffer(1)]],
    device const float* ddgiDepthBuffer [[buffer(2)]],
    texture2d<float, access::write> dynSHAtlas [[texture(0)]],
    texture2d<float, access::write> dynDepthAtlas [[texture(1)]]
) {
    if (gid >= params.probeCountTotal) return;

    uint3 c = params.probeCounts;
    uint px = gid % c.x;
    uint rem = gid / c.x;
    uint py = rem % c.y;
    uint pz = rem / c.y;

    uint shX = px * 4u;
    uint shY = py + pz * c.y;

    // Write CONSTANT green to ALL atlas texels (test atlas write path)
    for (uint i = 0u; i < 4u; ++i) {
        dynSHAtlas.write(float4(0.2f, 0.8f, 0.3f, 1.0f), uint2(shX + i, shY));
    }

    // Write constant to depth atlas too
    uint depthX = px * 8u;
    uint depthY = (py + pz * c.y) * 8u;
    for (uint j = 0u; j < 8u; ++j) {
        for (uint i = 0u; i < 8u; ++i) {
            dynDepthAtlas.write(float4(4.0f, 1.0f, 0.0f, 0.0f), uint2(depthX + i, depthY + j));
        }
    }
}
