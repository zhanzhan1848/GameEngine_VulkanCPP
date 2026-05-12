#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "../CommonFunction.metal"
#include "SurfaceCacheData.metal"

struct IndirectTraceParams {
    SurfaceCacheParams sc_params;
    uint tile_size;
    uint rays_per_probe;
    uint frame_index;
    float near_distance;
    float max_ray_distance;
    float4 sdf_origins[3];
    float4 sdf_voxel_sizes[3];
    float4 sdf_extents[3];
    uint sdf_resolutions[3];
    uint sdf_cascade_count;
};

struct ProbeRayHit {
    float3 hit_position;
    float  hit_distance;
    uint   hit_type;   // 0=near, 1=far, 2=sky
    float  _pad;
};

static float sampleSDF(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant IndirectTraceParams& params)
{
    float d = 1e6;
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    for (uint c = 0; c < params.sdf_cascade_count; ++c) {
        float3 local_p = (pos - params.sdf_origins[c].xyz) / params.sdf_extents[c].xyz;
        float3 uvw = local_p * 0.5 + 0.5;
        if (uvw.x < 0 || uvw.x > 1 || uvw.y < 0 || uvw.y > 1 || uvw.z < 0 || uvw.z > 1) continue;
        texture3d<float, access::sample> sdf_tex = (c == 0) ? sdf0 : ((c == 1) ? sdf1 : sdf2);
        float sd = sdf_tex.sample(s, uvw).r * params.sdf_extents[c].x;
        d = min(d, sd);
    }
    return d;
}

kernel void surfaceCacheIndirectTrace(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read>  depth_atlas  [[texture(0)]],
    texture2d<float, access::read>  normal_atlas [[texture(1)]],
    texture3d<float, access::sample> sdf0        [[texture(2)]],
    texture3d<float, access::sample> sdf1        [[texture(3)]],
    texture3d<float, access::sample> sdf2        [[texture(4)]],
    device GlobalShaderData&        gd           [[buffer(0)]],
    constant IndirectTraceParams&   params       [[buffer(1)]],
    constant SurfaceCacheCard*      cards        [[buffer(2)]],
    device ProbeRayHit*             ray_hits     [[buffer(3)]])
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    uint tiles_x = params.sc_params.atlas_size / params.tile_size;
    uint probe_idx = probe_y * tiles_x + probe_x;

    uint2 tile_origin = uint2(probe_x * params.tile_size + params.tile_size/2,
                               probe_y * params.tile_size + params.tile_size/2);
    if (tile_origin.x >= params.sc_params.atlas_size || tile_origin.y >= params.sc_params.atlas_size) return;

    float depth = depth_atlas.read(tile_origin).r;
    if (depth <= 0.0) return;

    float2 enc_n = normal_atlas.read(tile_origin).rg;
    float3 probe_normal = octDecode(enc_n);

    // Reconstruct world pos from card
    float3 probe_pos = cardTexelToWorld(tile_origin, depth, cards[0]);

    uint rays = params.rays_per_probe;
    for (uint r = 0; r < rays; ++r) {
        // Cosine-weighted hemisphere
        float2 seed = float2(hash(float2(float(probe_idx * rays + r), float(params.frame_index))),
                             hash(float2(float(params.frame_index), float(probe_idx * rays + r))));
        float r1 = seed.x;
        float r2 = seed.y * 2.0 * 3.14159265;
        float sin_t = sqrt(r1);
        float cos_t = sqrt(1.0 - r1);
        float3 local_dir = float3(cos_t * cos(r2), cos_t * sin(r2), sin_t);

        float3 up = abs(probe_normal.y) < 0.999 ? float3(0,1,0) : float3(1,0,0);
        float3 tangent = normalize(cross(up, probe_normal));
        float3 bitangent = cross(probe_normal, tangent);
        float3 ray_dir = normalize(tangent * local_dir.x + bitangent * local_dir.y + probe_normal * local_dir.z);

        float t = 0.0;
        bool hit = false;
        float3 hit_pos = float3(0.0);

        for (uint step = 0; step < 64; ++step) {
            float3 p = probe_pos + ray_dir * t;
            float d = sampleSDF(p, sdf0, sdf1, sdf2, params);
            if (d < 0.01) { hit = true; hit_pos = p; break; }
            if (t > params.max_ray_distance) break;
            t += max(d, 0.01);
        }

        uint hit_idx = probe_idx * rays + r;
        ray_hits[hit_idx].hit_distance = t;
        if (hit && t < params.near_distance) {
            ray_hits[hit_idx].hit_type = 0;
            ray_hits[hit_idx].hit_position = hit_pos;
        } else if (hit) {
            ray_hits[hit_idx].hit_type = 1;
            ray_hits[hit_idx].hit_position = hit_pos;
        } else {
            ray_hits[hit_idx].hit_type = 2;
            ray_hits[hit_idx].hit_position = float3(0.0);
        }
    }
}
