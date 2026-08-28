/**
 * @file DDGICardRadianceAvg.metal
 * @brief Samples a 4x4 texel grid across each card's area in the Surface Cache
 *        Lighting Atlas and writes a coverage-weighted average radiance. The w
 *        component carries the fraction of sampled texels that held any
 *        energy — DDGIProbeIrradianceFromCards uses it as per-card SC validity.
 *
 * Dispatch: (CardCount, 1, 1), threadGroupSize = (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "SurfaceCacheData.metal"

kernel void ddgi_card_radiance_avg(
    uint gid [[thread_position_in_grid]],

    constant SurfaceCacheParams& params [[buffer(0)]],
    device const SurfaceCacheCard* cards [[buffer(1)]],
    device float4* card_radiance        [[buffer(2)]],

    texture2d<float, access::sample> lighting_atlas [[texture(0)]]
) {
    if (gid >= params.max_cards) return;

    device const SurfaceCacheCard& card = cards[gid];
    if (card.resolution == 0) {
        card_radiance[gid] = float4(0.0);
        return;
    }

    // 4x4 grid across the card area (never-captured atlas texels read as the
    // init default zero, so "any energy" doubles as a captured-validity test).
    float res = float(card.resolution);
    float3 sum = float3(0.0);
    float valid = 0.0;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            float2 uv = float2(
                float(card.atlas_offset_x) + res * (float(i) + 0.5) / 4.0,
                float(card.atlas_offset_y) + res * (float(j) + 0.5) / 4.0
            ) / float(params.atlas_size);
            float3 r = lighting_atlas.sample(sampler(coord::normalized, filter::nearest,
                                                     address::clamp_to_edge), uv).xyz;
            if (dot(r, r) > 1e-6) {
                sum += r;
                valid += 1.0;
            }
        }
    }

    if (valid > 0.0) {
        card_radiance[gid] = float4(sum / valid, valid / 16.0);
    } else {
        card_radiance[gid] = float4(0.0);
    }
}
