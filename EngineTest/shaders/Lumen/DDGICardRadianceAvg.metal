/**
 * @file DDGICardRadianceAvg.metal
 * @brief Samples a 4x4 block grid across each card's area in the Surface Cache
 *        Lighting Atlas. Each block samples a 2x2 texel footprint and writes a
 *        coverage-weighted average radiance. The w component is block validity.
 *
 * Dispatch: (CardCount * 16, 1, 1), threadGroupSize = (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "Lumen/SurfaceCacheData.metal"

kernel void ddgi_card_radiance_avg(
    uint gid [[thread_position_in_grid]],

    constant SurfaceCacheParams& params [[buffer(0)]],
    device const SurfaceCacheCard* cards [[buffer(1)]],
    device float4* card_radiance        [[buffer(2)]],

    texture2d<float, access::sample> lighting_atlas [[texture(0)]]
) {
    uint cardIndex = gid / 16u;
    uint blockIndex = gid % 16u;
    if (cardIndex >= params.max_cards) return;

    device const SurfaceCacheCard& card = cards[cardIndex];
    if (card.resolution == 0) {
        card_radiance[gid] = float4(0.0);
        return;
    }

    uint blockX = blockIndex % 4u;
    uint blockY = blockIndex / 4u;
    // Each block covers one quarter of the card dimension and takes 2x2
    // samples. Never-captured texels read as zero and yield coverage zero.
    float res = float(card.resolution);
    float3 sum = float3(0.0);
    float valid = 0.0;
    for (uint j = 0u; j < 2u; ++j) {
        for (uint i = 0u; i < 2u; ++i) {
            float2 uv = float2(
                float(card.atlas_offset_x) + res * (float(blockX * 2u + i) + 0.5) / 8.0,
                float(card.atlas_offset_y) + res * (float(blockY * 2u + j) + 0.5) / 8.0
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
        card_radiance[gid] = float4(sum / valid, valid / 4.0);
    } else {
        card_radiance[gid] = float4(0.0);
    }
}
