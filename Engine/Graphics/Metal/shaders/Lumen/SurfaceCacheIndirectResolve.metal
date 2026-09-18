#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "SurfaceCacheData.metal"

struct ProbeRayHit {
    float3 hit_position;
    float  hit_distance;
    uint   hit_type;
    float  _pad;
};

struct IndirectResolveParams {
    SurfaceCacheParams sc_params;
    uint tile_size;
    uint rays_per_probe;
    float temporal_weight;
    uint frame_index;
    uint lookup_count;
    // B (plan §三.B): mirrors the trace refresh amortization — C++
    // IndirectResolveParamsGPU fields at offsets 68/72.
    uint refresh_stride;
    uint refresh_phase;
    // C (plan §三.C): the trace's SDF hit band — along-axis tolerance floor.
    float hit_band;
};

kernel void surfaceCacheIndirectResolve(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::sample> prev_lighting [[texture(0)]],
    texture2d<float, access::read>   albedo_atlas   [[texture(1)]],
    texture2d<float, access::sample> sky_cubemap    [[texture(2)]],
    texture2d<float, access::write>  indirect_out   [[texture(3)]],
    texture2d<float, access::read>   prev_indirect_out [[texture(6)]],  // T3.3 temporal EMA
    device GlobalShaderData&           gd           [[buffer(0)]],
    constant IndirectResolveParams&    params       [[buffer(1)]],
    constant SurfaceCacheCardLookup*   lookups      [[buffer(2)]],
    constant SurfaceCacheCard*         cards        [[buffer(3)]],
    device const ProbeRayHit*          ray_hits     [[buffer(4)]],
    device uint*                       tile_valid   [[buffer(5)]],   // B: per-tile validity bitmask
    texture2d<float, access::read>     depth_atlas  [[texture(7)]],  // C: capture validation
    texture2d<float, access::read>     normal_atlas [[texture(8)]])  // C: orientation
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    uint tiles_x = params.sc_params.atlas_size / params.tile_size;
    uint probe_idx = probe_y * tiles_x + probe_x;

    uint rays = params.rays_per_probe;
    uint tile_size = params.tile_size;
    uint2 tile_origin = uint2(probe_x * tile_size, probe_y * tile_size);

    if (tile_origin.x >= params.sc_params.atlas_size || tile_origin.y >= params.sc_params.atlas_size) return;

    // C (plan §三.C): the tile receiver normal for the contract's orientation
    // test — the captured card face must face the same side as the querying
    // surface (two-sided walls have one card per side).
    const uint2 centerTexel = tile_origin + uint2(tile_size / 2u);
    const float3 tileNormal = octDecode(normal_atlas.read(centerTexel).rg);

    // B (plan §三.B): per-tile validity — tiles outside this frame's refresh
    // phase carry history (if valid) or explicit zeros (A=0); their stale
    // ray_hits are never consumed. hit_type >= 3 is the INVALID sentinel.
    const bool refreshed = params.refresh_stride <= 1u ||
                           (probe_idx % params.refresh_stride) == params.refresh_phase;
    const uint tvWord = probe_idx >> 5u;
    const uint tvBit = 1u << (probe_idx & 31u);
    const bool tileWasValid = (tile_valid[tvWord] & tvBit) != 0u;

    float3 indirect = float3(0.0);
    float total_weight = 0.0;

    for (uint r = 0; r < rays && refreshed; ++r) {
        uint hit_idx = probe_idx * rays + r;
        float3 hit_pos = ray_hits[hit_idx].hit_position;
        uint hit_type = ray_hits[hit_idx].hit_type;
        float3 radiance = float3(0.0);

        // D (plan §三.D): 3 = cone-occluded — occluder proven, position
        // unknown. Count with ZERO radiance (no sky through the occluder, no
        // exact SC fetch at a fabricated position). ≥4 = INVALID sentinel.
        if (hit_type == 3u) {
            indirect += float3(0.0);
            total_weight += 1.0;
            continue;
        }
        if (hit_type >= 4u) continue;

        // 2026-09-11 Metal sync (C3.5 slice 1 parity): near AND far hits both
        // resolve through the atlas — the card lookup is position-based and
        // works at any distance. The old split sent every t≥0.25 m hit to a
        // Phase-A 0.02 placeholder, which made the whole tile path
        // transport-inert (with the trace stub fixed, virtually all legit
        // hits are "far").
        // C (plan §三.C): unified query contract over ALL bucket candidates —
        // capture validity, side orientation, decomposed along/perp error,
        // min-key selection (Vulkan twin parity). Valid black accepted.
        if (hit_type == 0u || hit_type == 1u) {
            bool found = false;
            float bestKey = 1e30f;
            for (uint li = 0; li < params.lookup_count && !found; ++li) {
                float3 mn = lookups[li].aabb_min.xyz;
                float3 mx = lookups[li].aabb_max.xyz;
                if (hit_pos.x < mn.x || hit_pos.x > mx.x ||
                    hit_pos.y < mn.y || hit_pos.y > mx.y ||
                    hit_pos.z < mn.z || hit_pos.z > mx.z) continue;
                for (uint ci = lookups[li].card_start;
                     ci < lookups[li].card_start + lookups[li].card_count; ++ci) {
                    float3 cand; float key;
                    if (scQueryCardCandidate(cards[ci], hit_pos, tileNormal,
                                             max(params.hit_band, 0.15f),
                                             depth_atlas, prev_lighting,
                                             float3(0.0f), cand, key)) {
                        if (key < bestKey) { bestKey = key; radiance = cand; found = true; }
                    }
                }
            }
            if (!found) continue;   // WRC 计划 Stream B3: 查询失败 = 无测量（0.02 灰占位符删除）
        } else {
            // Sky — same tier as DDGITraceFinalize's escape
            // (DDGI_SKY_COLOR × SkyColor.x with the 0.4 default intensity).
            radiance = float3(2.75f, 2.88f, 3.00f) * 0.4f;
        }

        indirect += radiance;
        total_weight += 1.0;
    }

    if (total_weight > 0.0) indirect /= total_weight;

    // B: consumed current hits — the tile's history is legitimate carry data.
    if (refreshed && (tile_valid[tvWord] & tvBit) == 0u) {
        tile_valid[tvWord] |= tvBit;
    }

    // T3.3: temporal accumulation — see Vulkan twin (mix with the previous
    // frame's slot; frame_index >= 2 guarantees history exists).
    const float tw = clamp(params.temporal_weight, 0.0f, 0.95f);
    const bool hasHistory = params.frame_index >= 2u;

    for (uint dy = 0; dy < tile_size; ++dy) {
        for (uint dx = 0; dx < tile_size; ++dx) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;
            if (!refreshed) {
                // B: no new estimate this frame — carry valid history
                // verbatim, otherwise explicit zeros with A=0.
                if (tileWasValid && hasHistory) {
                    float3 prev = prev_indirect_out.read(texel).rgb;
                    indirect_out.write(float4(prev, 1.0), texel);
                } else {
                    indirect_out.write(float4(0.0), texel);
                }
                continue;
            }
            float3 albedo = albedo_atlas.read(texel).rgb;
            float3 current = indirect * albedo;
            if (hasHistory && tw > 0.0f) {
                float3 prev = prev_indirect_out.read(texel).rgb;
                current = mix(current, prev, tw);
            }
            // WRC 计划 Stream B3: 零有效测量 → A=0 显式无效（Vulkan 孪生同步）。
            indirect_out.write(float4(current, total_weight > 0.0f ? 1.0f : 0.0f), texel);
        }
    }
}
