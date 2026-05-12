/**
 * @file DDGICardRadianceAvg.metal
 * @brief Reads the Surface Cache Lighting Atlas at each card's center texel
 *        and writes a per-card float3 average radiance to a compact buffer.
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
    device float3* card_radiance        [[buffer(2)]],

    texture2d<float, access::sample> lighting_atlas [[texture(0)]]
) {
    if (gid >= params.max_cards) return;

    auto& card = cards[gid];
    if (card.resolution == 0) {
        card_radiance[gid] = float3(0.0);
        return;
    }

    // Sample lighting atlas at card center texel
    float2 atlas_uv = float2(
        float(card.atlas_offset_x) + float(card.resolution) * 0.5,
        float(card.atlas_offset_y) + float(card.resolution) * 0.5
    );
    float2 texCoord = atlas_uv / float(params.atlas_size);

    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
    float3 radiance = lighting_atlas.sample(samp, texCoord).xyz;

    card_radiance[gid] = radiance;
}
