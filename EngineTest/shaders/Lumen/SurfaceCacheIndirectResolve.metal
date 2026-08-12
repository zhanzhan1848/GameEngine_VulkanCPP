#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

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
};

kernel void surfaceCacheIndirectResolve(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::sample> prev_lighting [[texture(0)]],
    texture2d<float, access::read>   albedo_atlas   [[texture(1)]],
    texture2d<float, access::sample> sky_cubemap    [[texture(2)]],
    texture2d<float, access::write>  indirect_out   [[texture(3)]],
    device GlobalShaderData&           gd           [[buffer(0)]],
    constant IndirectResolveParams&    params       [[buffer(1)]],
    constant SurfaceCacheCardLookup*   lookups      [[buffer(2)]],
    constant SurfaceCacheCard*         cards        [[buffer(3)]],
    device const ProbeRayHit*          ray_hits     [[buffer(4)]])
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    uint tiles_x = params.sc_params.atlas_size / params.tile_size;
    uint probe_idx = probe_y * tiles_x + probe_x;

    uint rays = params.rays_per_probe;
    uint tile_size = params.tile_size;
    uint2 tile_origin = uint2(probe_x * tile_size, probe_y * tile_size);

    if (tile_origin.x >= params.sc_params.atlas_size || tile_origin.y >= params.sc_params.atlas_size) return;

    constexpr sampler linear_samp(coord::normalized, filter::linear, address::clamp_to_edge);

    float3 indirect = float3(0.0);
    float total_weight = 0.0;

    for (uint r = 0; r < rays; ++r) {
        uint hit_idx = probe_idx * rays + r;
        float3 hit_pos = ray_hits[hit_idx].hit_position;
        uint hit_type = ray_hits[hit_idx].hit_type;
        float3 radiance = float3(0.0);

        if (hit_type == 0) {
            // Near hit: lookup atlas UV through card system
            float2 atlas_uv;
            bool found = false;
            for (uint li = 0; li < params.lookup_count && !found; ++li) {
                float3 mn = lookups[li].aabb_min.xyz;
                float3 mx = lookups[li].aabb_max.xyz;
                if (hit_pos.x < mn.x || hit_pos.x > mx.x ||
                    hit_pos.y < mn.y || hit_pos.y > mx.y ||
                    hit_pos.z < mn.z || hit_pos.z > mx.z) continue;
                for (uint ci = lookups[li].card_start;
                     ci < lookups[li].card_start + lookups[li].card_count && !found; ++ci) {
                    found = worldToCardUV(hit_pos, cards[ci], atlas_uv);
                }
            }
            if (found) {
                float2 uv_norm = atlas_uv / float2(params.sc_params.atlas_size);
                radiance = prev_lighting.sample(linear_samp, uv_norm).rgb;
            } else {
                radiance = float3(0.02, 0.02, 0.03);
            }
        } else if (hit_type == 1) {
            // Far hit: DDGI placeholder
            radiance = float3(0.02, 0.02, 0.03);
        } else {
            // Sky
            radiance = float3(0.05, 0.05, 0.08);
        }

        indirect += radiance;
        total_weight += 1.0;
    }

    if (total_weight > 0.0) indirect /= total_weight;

    for (uint dy = 0; dy < tile_size; ++dy) {
        for (uint dx = 0; dx < tile_size; ++dx) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;
            float3 albedo = albedo_atlas.read(texel).rgb;
            indirect_out.write(float4(indirect * albedo, 1.0), texel);
        }
    }
}
