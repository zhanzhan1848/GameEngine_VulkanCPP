/**
 * @file SurfaceCacheShadow.metal
 * @brief T3.3 — per-texel directional light shadow via GlobalSDF march.
 *
 * Metal port of SurfaceCacheShadow.comp (canonical). Per card texel:
 * reconstruct world position from the depth atlas, march toward the light,
 * write visibility into the atlas-space float buffer consumed by LightEval
 * in a separate buffer-only dispatch.
 *
 * Dispatch: 1D over the budget-selected flattened texel range.
 * ThreadGroupSize: (256, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

struct SCShadowParams {
    float4  sdf_origins[3];
    float4  sdf_extents[3];
    float4  sdf_resolutions_count;   // xyz = res0/1/2, w = cascade count
    float4  light_direction;         // xyz = dir TOWARD the light, w unused
    float4  shadow_params;           // x = max_distance, y = normal_bias, z = atlas_size, w unused
    float4  counts;                  // x = total_texels, y = card_count
};

static bool scShadowInside(float3 pos, float3 origin, float3 extent) {
    return all(pos >= origin) && all(pos < origin + extent);
}

static float scShadowSample(texture3d<float, access::sample> tex, float3 pos,
                            float3 origin, float3 extent, uint res) {
    float3 uvw = (pos - origin) / extent;
    if (uvw.x < 0.0f || uvw.x > 1.0f || uvw.y < 0.0f || uvw.y > 1.0f ||
        uvw.z < 0.0f || uvw.z > 1.0f) return 1.0e10f;
    int r = int(res);
    int3 t = clamp(int3(uvw * float(r)), int3(0), int3(r - 1));
    return tex.read(uint3(t)).r;
}

kernel void surfaceCacheShadow(
    uint global_tid [[thread_position_in_grid]],

    texture3d<float, access::sample> sdf0 [[texture(0)]],
    texture3d<float, access::sample> sdf1 [[texture(1)]],
    texture3d<float, access::sample> sdf2 [[texture(2)]],
    texture2d<float, access::read>   depth_atlas  [[texture(3)]],
    texture2d<float, access::read>   normal_atlas [[texture(4)]],

    constant SCShadowParams& sp           [[buffer(0)]],
    constant CardDispatchInfo* card_dispatch [[buffer(1)]],
    constant SurfaceCacheCard* cards      [[buffer(2)]],
    device float* visibility              [[buffer(3)]])
{
    if (global_tid >= uint(sp.counts.x)) return;

    uint lo = 0u;
    uint hi = uint(sp.counts.y);
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (card_dispatch[mid].texel_offset + card_dispatch[mid].texel_count <= global_tid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo >= uint(sp.counts.y)) return;

    uint local_texel = global_tid - card_dispatch[lo].texel_offset;
    uint res = card_dispatch[lo].resolution;
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_uv = uint2(
        card_dispatch[lo].atlas_offset_x + local_x,
        card_dispatch[lo].atlas_offset_y + local_y);
    uint card_idx = card_dispatch[lo].card_index;

    uint atlas_size = uint(sp.shadow_params.z);
    uint vis_index = atlas_uv.x + atlas_size * atlas_uv.y;

    float depth = depth_atlas.read(atlas_uv).r;
    if (depth <= 0.0f) {
        visibility[vis_index] = 1.0f;
        return;
    }

    float2 enc_n = normal_atlas.read(atlas_uv).rg;
    float3 normal = octDecode(enc_n);

    float3 world = cardTexelToWorld(atlas_uv, depth, cards[card_idx]);

    uint cc = uint(sp.sdf_resolutions_count.w);
    float voxel = cc > 0u
        ? max(sp.sdf_extents[0].x / max(sp.sdf_resolutions_count.x, 1.0f), 0.01f)
        : 1.0f;
    float3 origin = world + normal * (voxel * sp.shadow_params.y);

    float3 L = sp.light_direction.xyz;
    float maxDist = sp.shadow_params.x;
    float t = voxel;
    float hitThreshold = voxel * 0.5f;
    float shadow = 1.0f;

    for (int step = 0; step < 16; ++step) {
        float3 pos = origin + L * t;
        float d = 1.0e10f;
        if (cc > 0u && scShadowInside(pos, sp.sdf_origins[0].xyz, sp.sdf_extents[0].xyz))
            d = scShadowSample(sdf0, pos, sp.sdf_origins[0].xyz, sp.sdf_extents[0].xyz,
                               uint(sp.sdf_resolutions_count.x));
        else if (cc > 1u && scShadowInside(pos, sp.sdf_origins[1].xyz, sp.sdf_extents[1].xyz))
            d = scShadowSample(sdf1, pos, sp.sdf_origins[1].xyz, sp.sdf_extents[1].xyz,
                               uint(sp.sdf_resolutions_count.y));
        else if (cc > 2u && scShadowInside(pos, sp.sdf_origins[2].xyz, sp.sdf_extents[2].xyz))
            d = scShadowSample(sdf2, pos, sp.sdf_origins[2].xyz, sp.sdf_extents[2].xyz,
                               uint(sp.sdf_resolutions_count.z));

        if (d > 1.0e9f) break;   // left all cascades → unoccluded
        if (d < hitThreshold) { shadow = 0.0f; break; }
        float advance = (d > hitThreshold * 4.0f) ? d * 1.2f : max(d, voxel * 0.25f);
        t += advance;
        if (t >= maxDist) break;
    }

    visibility[vis_index] = shadow;
}
