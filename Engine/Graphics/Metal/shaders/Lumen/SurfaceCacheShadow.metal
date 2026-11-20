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
    // G2（2026-09-16，与 GLSL 孪生同步）: PCSS 矩阵 + meta
    float4  shadow_matrices[4][4];   // [cascade][column] lightProj*lightView
    float4  shadow_meta;             // x=cascade_count（>0.5 启用）, y=normal_offset_scale
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

// Match frozen SurfaceCacheShadow.comp: choose the finest containing
// cascade, and scale the receiver bias/threshold by its actual voxel size.
static float scShadowSampleBest(float3 pos, constant SCShadowParams& sp,
    texture3d<float, access::sample> sdf0, texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2) {
    uint cc=uint(sp.sdf_resolutions_count.w);
    if(cc>0u && scShadowInside(pos,sp.sdf_origins[0].xyz,sp.sdf_extents[0].xyz))
        return scShadowSample(sdf0,pos,sp.sdf_origins[0].xyz,sp.sdf_extents[0].xyz,uint(sp.sdf_resolutions_count.x));
    if(cc>1u && scShadowInside(pos,sp.sdf_origins[1].xyz,sp.sdf_extents[1].xyz))
        return scShadowSample(sdf1,pos,sp.sdf_origins[1].xyz,sp.sdf_extents[1].xyz,uint(sp.sdf_resolutions_count.y));
    if(cc>2u && scShadowInside(pos,sp.sdf_origins[2].xyz,sp.sdf_extents[2].xyz))
        return scShadowSample(sdf2,pos,sp.sdf_origins[2].xyz,sp.sdf_extents[2].xyz,uint(sp.sdf_resolutions_count.z));
    return 1.0e10f;
}
static float scShadowVoxelAt(float3 pos, constant SCShadowParams& sp) {
    uint cc=uint(sp.sdf_resolutions_count.w);
    for(uint c=0u;c<cc && c<3u;++c)
        if(scShadowInside(pos,sp.sdf_origins[c].xyz,sp.sdf_extents[c].xyz))
            return max(sp.sdf_extents[c].x/max(sp.sdf_resolutions_count[c],1.0f),0.01f);
    return cc>0u?max(sp.sdf_extents[0].x/max(sp.sdf_resolutions_count.x,1.0f),0.01f):1.0f;
}

// G2: PCSS 4-tap PCF（与 GLSL scPcf4 同式；access::read 用 read() 采样）
static float scPcf4Depth(texture2d<float, access::read> depthMap,
                         float2 uv, float ndcZ) {
    uint2 dims = uint2(depthMap.get_width(), depthMap.get_height());
    const float bias = 0.0015f;
    float2 q = clamp(uv, float2(0.0f), float2(1.0f)) * float2(dims) - 0.5f;
    int2 t0 = int2(floor(q));
    int2 t1 = min(t0 + int2(1), int2(dims) - int2(1));
    t0 = max(t0, int2(0));
    float lit = 0.0f;
    lit += (ndcZ - bias > depthMap.read(uint2(t0)).r) ? 0.0f : 1.0f;
    lit += (ndcZ - bias > depthMap.read(uint2(t1.x, t0.y)).r) ? 0.0f : 1.0f;
    lit += (ndcZ - bias > depthMap.read(uint2(t0.x, t1.y)).r) ? 0.0f : 1.0f;
    lit += (ndcZ - bias > depthMap.read(uint2(t1)).r) ? 0.0f : 1.0f;
    return lit * 0.25f;
}

static float scShadowMapVisibility(float3 world, float3 normal, float footprint,
                                   constant SCShadowParams& sp,
                                   texture2d<float, access::read> d0,
                                   texture2d<float, access::read> d1,
                                   texture2d<float, access::read> d2,
                                   texture2d<float, access::read> d3) {
    uint cascadeCount = uint(sp.shadow_meta.x + 0.5f);
    float3 p = world + normal * (footprint * sp.shadow_meta.y);
    for (uint c = 0u; c < cascadeCount && c < 4u; ++c) {
        float4 clip = sp.shadow_matrices[c][0] * p.x
                    + sp.shadow_matrices[c][1] * p.y
                    + sp.shadow_matrices[c][2] * p.z
                    + sp.shadow_matrices[c][3];
        if (clip.w <= 1e-6f) continue;
        float3 ndc = clip.xyz / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f ||
            ndc.y < -1.0f || ndc.y > 1.0f ||
            ndc.z <  0.0f || ndc.z > 1.0f) continue;
        float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (c == 0u) return scPcf4Depth(d0, uv, ndc.z);
        if (c == 1u) return scPcf4Depth(d1, uv, ndc.z);
        if (c == 2u) return scPcf4Depth(d2, uv, ndc.z);
        return scPcf4Depth(d3, uv, ndc.z);
    }
    return -1.0f;   // 全级联越界 → SDF march 回退
}
kernel void surfaceCacheShadow(
    uint global_tid [[thread_position_in_grid]],

    texture3d<float, access::sample> sdf0 [[texture(0)]],
    texture3d<float, access::sample> sdf1 [[texture(1)]],
    texture3d<float, access::sample> sdf2 [[texture(2)]],
    texture2d<float, access::read>   depth_atlas  [[texture(3)]],
    texture2d<float, access::read>   normal_atlas [[texture(4)]],
    texture2d<float, access::read>   shadow_depth_0 [[texture(5)]],
    texture2d<float, access::read>   shadow_depth_1 [[texture(6)]],
    texture2d<float, access::read>   shadow_depth_2 [[texture(7)]],
    texture2d<float, access::read>   shadow_depth_3 [[texture(8)]],

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

    // G2: PCSS 深度优先——与表面直射同一份阴影（薄墙/细柱正确）；越界或
    // 无深度输入走 SDF march 回退（与 GLSL 孪生同语义）。
    if (sp.shadow_meta.x > 0.5f) {
        float texelRes = float(max(uint(cards[card_idx].resolution), 1u));
        float footprint = length(cards[card_idx].extent.xyz) / texelRes;
        float v = scShadowMapVisibility(world, normal, footprint, sp,
                                        shadow_depth_0, shadow_depth_1,
                                        shadow_depth_2, shadow_depth_3);
        if (v >= 0.0f) {
            visibility[vis_index] = v;
            return;
        }
    }

    float voxel = scShadowVoxelAt(world,sp);
    float3 origin = world + normal * (voxel * sp.shadow_params.y);
    float originDistance = scShadowSampleBest(origin,sp,sdf0,sdf1,sdf2);
    float3 L = sp.light_direction.xyz;
    float maxDist = sp.shadow_params.x;
    float t = voxel;
    // An unsigned SDF close to an unrelated surface is not itself a blocker.
    // The frozen reference requires the march to get closer than half the
    // origin distance, bounded by half the receiver's cascade voxel.
    float hitThreshold = min(voxel*0.5f,max(originDistance*0.5f,0.0f));
    float shadow = 1.0f;
    for (int step = 0; step < 16; ++step) {
        float d = scShadowSampleBest(origin+L*t,sp,sdf0,sdf1,sdf2);
        if (d > 1.0e9f) break;
        if (d < hitThreshold) { shadow=0.0f; break; }
        float advance = d>hitThreshold*4.0f?d*1.2f:max(d,voxel*0.25f);
        t += advance;
        if (t >= maxDist) break;
    }
    visibility[vis_index] = shadow;
}
