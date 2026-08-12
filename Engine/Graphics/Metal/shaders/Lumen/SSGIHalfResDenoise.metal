/**
 * @file SSGIHalfResDenoise.metal
 * @brief SSGI half-resolution Gaussian pre-smooth before bilinear upsampling
 *
 * Pure 5x5 Gaussian blur at half-res. No edge-stopping weights —
 * edge preservation is handled by the full-res bilateral spatial filter.
 *
 * Dispatch: (halfW + 7)/8, (halfH + 7)/8, 1
 * ThreadGroupSize: (8, 8, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct HalfResDenoiseParams {
    uint  width;
    uint  height;
    float sigma;
    float pad[5];
};

kernel void ssgi_halfres_denoise(
    texture2d<float, access::read>  trace_input  [[texture(0)]],
    texture2d<float, access::write> trace_output [[texture(1)]],

    constant HalfResDenoiseParams& params [[buffer(0)]],

    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float invTwoSigmaSq = 1.0 / (2.0 * params.sigma * params.sigma);

    float3 filteredIrr  = float3(0.0);
    float  filteredDist = 0.0;
    float  totalWeight  = 0.0;

    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int2 pos = int2(gid) + int2(dx, dy);
            if (pos.x < 0 || pos.y < 0 ||
                pos.x >= int(params.width) || pos.y >= int(params.height)) {
                continue;
            }

            float4 s = trace_input.read(uint2(pos));

            float spatial2 = float(dx * dx + dy * dy);
            float w = exp(-spatial2 * invTwoSigmaSq);

            filteredIrr  += s.rgb * w;
            filteredDist += s.a   * w;
            totalWeight  += w;
        }
    }

    if (totalWeight > 0.001) {
        trace_output.write(float4(filteredIrr / totalWeight,
                                   filteredDist / totalWeight), gid);
    } else {
        trace_output.write(trace_input.read(gid), gid);
    }
}
