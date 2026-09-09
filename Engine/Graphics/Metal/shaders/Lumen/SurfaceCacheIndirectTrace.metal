#include <metal_stdlib>
using namespace metal;
#include "../CommonTypes.metal"
#include "../CommonFunction.metal"
#include "SurfaceCacheData.metal"
#include "SDFTraceCommon.metal"

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

static uint findOwningCard(uint2 uv, constant SurfaceCacheCard* cards, uint count) {
    // Apple Silicon: cap device reads to ~24 to stay within limits
    uint limit = min(count, 24u);
    for (uint i = 0; i < limit; ++i) {
        if (uv.x >= cards[i].atlas_offset_x && uv.x < cards[i].atlas_offset_x + cards[i].resolution &&
            uv.y >= cards[i].atlas_offset_y && uv.y < cards[i].atlas_offset_y + cards[i].resolution)
            return i;
    }
    return count;
}

// Apple Silicon: else-if cascade selection = 1 texture3D read per step (not 3)
static float sampleSDF(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant IndirectTraceParams& params)
{
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);

    if (params.sdf_cascade_count > 0) {
        float3 local_p = (pos - params.sdf_origins[0].xyz) / params.sdf_extents[0].xyz;
        float3 uvw = local_p * 0.5 + 0.5;
        if (uvw.x >= 0 && uvw.x <= 1 && uvw.y >= 0 && uvw.y <= 1 && uvw.z >= 0 && uvw.z <= 1)
            return sdf0.sample(s, uvw).r * params.sdf_extents[0].x;
    }
    if (params.sdf_cascade_count > 1) {
        float3 local_p = (pos - params.sdf_origins[1].xyz) / params.sdf_extents[1].xyz;
        float3 uvw = local_p * 0.5 + 0.5;
        if (uvw.x >= 0 && uvw.x <= 1 && uvw.y >= 0 && uvw.y <= 1 && uvw.z >= 0 && uvw.z <= 1)
            return sdf1.sample(s, uvw).r * params.sdf_extents[1].x;
    }
    if (params.sdf_cascade_count > 2) {
        float3 local_p = (pos - params.sdf_origins[2].xyz) / params.sdf_extents[2].xyz;
        float3 uvw = local_p * 0.5 + 0.5;
        if (uvw.x >= 0 && uvw.x <= 1 && uvw.y >= 0 && uvw.y <= 1 && uvw.z >= 0 && uvw.z <= 1)
            return sdf2.sample(s, uvw).r * params.sdf_extents[2].x;
    }
    return 1e6;
}

kernel void surfaceCacheIndirectTrace(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read>  depth_atlas  [[texture(0)]],
    texture2d<float, access::read>  normal_atlas [[texture(1)]],
    texture3d<float, access::sample> sdf0        [[texture(2)]],
    texture3d<float, access::sample> sdf1        [[texture(3)]],
    texture3d<float, access::sample> sdf2        [[texture(4)]],
    // T3.2 (D7): cascade-0 conservative min-mip chain (DispatchMipChain).
    // Missing slots rebind sdf0 itself → width comparison self-gates the
    // cone hit test off (PRIMAL_SDF_CONE_HIT=0 kill switch is CPU-side).
    texture3d<float, access::read>  sdf0_mip0    [[texture(5)]],
    texture3d<float, access::read>  sdf0_mip1    [[texture(6)]],
    texture3d<float, access::read>  sdf0_mip2    [[texture(7)]],
    device GlobalShaderData&        gd           [[buffer(0)]],
    constant IndirectTraceParams&   params       [[buffer(1)]],
    constant SurfaceCacheCard*      cards        [[buffer(2)]],
    device ProbeRayHit*             ray_hits     [[buffer(3)]])
{
    uint probe_x = global_id.x;
    uint probe_y = global_id.y;
    if (params.tile_size == 0) return;
    uint tiles_x = params.sc_params.atlas_size / params.tile_size;
    uint probe_idx = probe_y * tiles_x + probe_x;

    uint2 tile_origin = uint2(probe_x * params.tile_size + params.tile_size/2,
                               probe_y * params.tile_size + params.tile_size/2);
    if (tile_origin.x >= params.sc_params.atlas_size || tile_origin.y >= params.sc_params.atlas_size) return;

    float depth = depth_atlas.read(tile_origin).r;
    if (depth <= 0.0) return;

    float2 enc_n = normal_atlas.read(tile_origin).rg;
    float3 probe_normal = octDecode(enc_n);

    // Find owning card for correct world position
    uint card_idx = findOwningCard(tile_origin, cards, params.sc_params.max_cards);
    if (card_idx >= params.sc_params.max_cards) return;
    float3 probe_pos = cardTexelToWorld(tile_origin, depth, cards[card_idx]);

    uint rays = params.rays_per_probe;
    // Apple Silicon: 4 SDF steps × 1 texture3D read/step = 4 reads/ray (within ~4 limit)
    constexpr uint kMaxSDFSteps = 4;
    for (uint r = 0; r < rays; ++r) {
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

        // Adaptive threshold based on finest cascade voxel size
        float hitThreshold = params.sdf_extents[0].x * 0.5f;
        float minStep = params.sdf_extents[0].x * 0.25f;

        for (uint step = 0; step < kMaxSDFSteps; ++step) {
            float3 p = probe_pos + ray_dir * t;
            float d = sampleSDF(p, sdf0, sdf1, sdf2, params);

            if (d < hitThreshold) { hit = true; hit_pos = p; break; }
            if (t > params.max_ray_distance) break;

            // Relaxed advancement: overstep when far, conservative when close
            float advance;
            if (d > hitThreshold * 4.0f) {
                advance = d * 1.2f;
            } else {
                advance = max(d, minStep);
            }
            t += advance;
        }

        uint hit_idx = probe_idx * rays + r;
        // T3.2 (D7) → T3.3 consumer: when the 4-step march exhausts (previously
        // classified sky), test the min-mip chain with the ray's widening cone
        // footprint — geometry in the cone → far occlusion (no sky radiance).
        if (!hit) {
            const float3 org = params.sdf_origins[0].xyz;
            const float3 ext = params.sdf_extents[0].xyz;
            // Ray-AABB slab: clamp the sample t into the cascade volume — the
            // exhausted-ray endpoint is usually OUTSIDE it, and the unclamped
            // read hits the 1e10 sentinel so the test would never fire.
            const float3 invD = select(float3(1e8f), 1.0f / ray_dir,
                                       abs(ray_dir) > float3(1e-8f));
            const float3 t0s = (org - probe_pos) * invD;
            const float3 t1s = (org + ext - probe_pos) * invD;
            const float3 tsm = min(t0s, t1s);
            const float3 tbg = max(t0s, t1s);
            const float tEnter = max(max(tsm.x, tsm.y), max(tsm.z, 0.0f));
            const float tExit = min(min(tbg.x, tbg.y), tbg.z);
            const float sampleT = min(min(t, params.max_ray_distance), tExit);
            if (tExit > tEnter && sampleT > tEnter) {
                const float3 samplePos = probe_pos + ray_dir * sampleT;
                const uint baseRes = sdf0.get_width();
                const uint m0Res = sdf0_mip0.get_width();
                if (baseRes > 0u && m0Res > 0u && m0Res < baseRes) {
                    // cosine-lobe per-ray half-angle ≈ sqrt(4π/N)
                    const float rayAngle = sqrt(12.566371f / float(max(rays, 1u)));
                    const float footprint = sampleT * rayAngle;
                    if (footprint > 0.0f) {
                        const float voxel0 = ext.x / float(baseRes);
                        const float2 lf = coneMipFrac(footprint, voxel0, 3u);
                        texture3d<float, access::read> mipTex =
                            lf.x < 0.5f ? sdf0_mip0 : (lf.x < 1.5f ? sdf0_mip1 : sdf0_mip2);
                        if (mipTex.get_width() > 0u && mipTex.get_width() < baseRes) {
                            const float mipD = sampleSDFCascadeMip(mipTex, samplePos, org, ext);
                            if (mipD < footprint * 0.5f) {
                                hit = true;
                                hit_pos = probe_pos + ray_dir * min(t, params.max_ray_distance);
                            }
                        }
                    }
                }
            }
        }
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
