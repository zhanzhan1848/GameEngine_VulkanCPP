// Vulkan reference: Lumen/SurfaceCacheIndirectTrace.comp (surface_cache_indirect_trace).
#include "../Common/TextureDimensions.hlsli"

struct SurfaceCacheCard
{
    float4 center;
    float4 extent;
    uint axis_direction;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint mesh_instance_id;
    uint static_mesh_slot;
    uint flags;
    uint page_card;
};

struct SurfaceCacheParams
{
    uint atlas_size;
    uint page_size;
    uint capture_budget_pages;
    uint max_cards;
    float update_distance;
    float importance_weight;
    uint max_lights_per_tile;
    uint indirect_rays_per_probe;
    float indirect_temporal_weight;
    float indirect_near_distance;
    uint lookup_count;
    uint _pad;
};

struct IndirectTraceParams
{
    SurfaceCacheParams sc_params;
    uint tile_size;
    uint rays_per_probe;
    uint frame_index;
    float near_distance;
    float max_ray_distance;
    uint refresh_stride;
    uint refresh_phase;
    float4 sdf_origins[3];
    float4 sdf_voxel_sizes[3];
    float4 sdf_extents[3];
    uint sdf_resolutions[3];
    uint4 sdf_cascade_count;
};

struct ProbeRayHit
{
    float4 hit_position_dist;
    uint4 hit_type_pad;
};

cbuffer IndirectTraceParamsBlock : register(b6, space0)
{
    IndirectTraceParams itp_params : packoffset(c0);
};

RWByteAddressBuffer ray_hits : register(u8, space0);
ByteAddressBuffer owners : register(t12, space0);
ByteAddressBuffer cards : register(t7, space0);
cbuffer GDBlock : register(b5, space0)
{
    row_major float4x4 gd_view : packoffset(c0);
    row_major float4x4 gd_projection : packoffset(c4);
    row_major float4x4 gd_invProjection : packoffset(c8);
    row_major float4x4 gd_viewProjection : packoffset(c12);
    row_major float4x4 gd_previousViewProjection : packoffset(c16);
    row_major float4x4 gd_invViewProjection : packoffset(c20);
    float4 gd_cameraPositionAndViewWidth : packoffset(c24);
    float4 gd_cameraDirectionAndViewHeight : packoffset(c25);
    uint gd_numDirectionalLights : packoffset(c26);
    uint gd_numPunctualLights : packoffset(c26.y);
    float gd_deltaTime : packoffset(c26.z);
    float gd_frameCount : packoffset(c26.w);
    uint gd_renderMode : packoffset(c27);
    uint gd_enableIBL : packoffset(c27.y);
    uint gd_enableDDGI : packoffset(c27.z);
    uint gd_pad_before_jitter : packoffset(c27.w);
    float2 gd_jitterOffset : packoffset(c28);
    float gd_debug_directLightBoost : packoffset(c28.z);
    float gd_debug_iblStrength : packoffset(c28.w);
    float gd_debug_ddgiIndirectWeight : packoffset(c29);
    float gd_debug_exposure : packoffset(c29.y);
    float gd_pad_after_debug_0 : packoffset(c29.z);
    float gd_pad_after_debug_1 : packoffset(c29.w);
};

Texture3D<float4> sdf0 : register(t2, space0);
Texture3D<float4> sdf1 : register(t3, space0);
Texture3D<float4> sdf2 : register(t4, space0);
RWTexture2D<float> depth_atlas : register(u0, space0);
RWTexture2D<float2> normal_atlas : register(u1, space0);
Texture3D<float4> sdf0_mip0 : register(t9, space0);
Texture3D<float4> sdf0_mip1 : register(t10, space0);
Texture3D<float4> sdf0_mip2 : register(t11, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 octDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    // Inverse of the replace-fold in octEncode: n.xy >= 0 subtracts t, else adds.
    n.xy += lerp((float2)(t), (float2)(-t), step((float2)(0.0), n.xy));
    return normalize(n);
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card) {    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    float3 local_pos;
    if (axis == 0u) {
        local_pos.x = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (axis == 1u) {
        local_pos.y = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else {
        local_pos.z = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center.xyz;
}

uint scTileHash32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float2 scTileRayXi(uint tileIndex, uint rayIndex, uint frameIndex)
{
    uint seed = ((tileIndex * 2654435769u) ^ (rayIndex * 2246822507u)) ^ (frameIndex * 3266489909u);
    uint param = seed ^ 1757159915u;
    uint _287 = scTileHash32(param);
    uint param_1 = seed ^ 48610963u;
    uint _295 = scTileHash32(param_1);
    return float2(float(_287 >> 8u), float(_295 >> 8u)) * 5.9604645e-08f;
}

float sampleSDFNearest(Texture3D<float4> sdf, float3 uvw)
{

    int3 size = int3(textureDimensions(sdf, 0u));
    int3 coord = int3(clamp(uvw, 0.0f.xxx, 1.0f.xxx) * float3(size));
    return sdf.Load(int4(clamp(coord, int3(0, 0, 0), size - int3(1, 1, 1)), 0)).x;
}

float sampleSDF(float3 pos)
{
    if (itp_params.sdf_cascade_count.x > 0u)
    {
        float3 uvw = (pos - itp_params.sdf_origins[0].xyz) / itp_params.sdf_extents[0].xyz;
        if (((((((uvw.x >= 0.0f) && (uvw.x <= 1.0f)) && (uvw.y >= 0.0f)) && (uvw.y <= 1.0f)) && (uvw.z >= 0.0f)) && (uvw.z <= 1.0f)))
        {
            float3 param = uvw;
            return sampleSDFNearest(sdf0, param);
        }
    }
    if (itp_params.sdf_cascade_count.x > 1u)
    {
        float3 uvw_1 = (pos - itp_params.sdf_origins[1].xyz) / itp_params.sdf_extents[1].xyz;
        if (((((((uvw_1.x >= 0.0f) && (uvw_1.x <= 1.0f)) && (uvw_1.y >= 0.0f)) && (uvw_1.y <= 1.0f)) && (uvw_1.z >= 0.0f)) && (uvw_1.z <= 1.0f)))
        {
            float3 param_1 = uvw_1;
            return sampleSDFNearest(sdf1, param_1);
        }
    }
    if (itp_params.sdf_cascade_count.x > 2u)
    {
        float3 uvw_2 = (pos - itp_params.sdf_origins[2].xyz) / itp_params.sdf_extents[2].xyz;
        if (((((((uvw_2.x >= 0.0f) && (uvw_2.x <= 1.0f)) && (uvw_2.y >= 0.0f)) && (uvw_2.y <= 1.0f)) && (uvw_2.z >= 0.0f)) && (uvw_2.z <= 1.0f)))
        {
            float3 param_2 = uvw_2;
            return sampleSDFNearest(sdf2, param_2);
        }
    }
    return 1000000.0f;
}

float coneMipLevelGLSL(float footprint, float voxelSize, float maxMip) {
    return floor(clamp(log2(max(footprint / (voxelSize * 2.0), 1.0)), 0.0, maxMip - 1.0));
}

float sampleSDFCascadeMipGLSL(Texture3D<float4> mipTex, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (((any(bool3(uvw.x < 0.0f.xxx.x, uvw.y < 0.0f.xxx.y, uvw.z < 0.0f.xxx.z))) || (any(bool3(uvw.x > 1.0f.xxx.x, uvw.y > 1.0f.xxx.y, uvw.z > 1.0f.xxx.z)))))
    {
        return 10000000000.0f;
    }

    int3 size = int3(textureDimensions(mipTex, 0u));
    float3 p = (clamp(uvw, 0.0f.xxx, 1.0f.xxx) * float3(size)) - 0.5f.xxx;
    int3 base = int3(clamp(floor(p), 0.0f.xxx, float3(size) - 1.0f.xxx));
    float3 f = frac(p);
    int3 c1 = min((base + int3(1, 1, 1)), (size - int3(1, 1, 1)));
    float d000 = mipTex.Load(int4(int3(base.x, base.y, base.z), 0)).x;
    float d100 = mipTex.Load(int4(int3(c1.x, base.y, base.z), 0)).x;
    float d010 = mipTex.Load(int4(int3(base.x, c1.y, base.z), 0)).x;
    float d110 = mipTex.Load(int4(int3(c1.x, c1.y, base.z), 0)).x;
    float d001 = mipTex.Load(int4(int3(base.x, base.y, c1.z), 0)).x;
    float d101 = mipTex.Load(int4(int3(c1.x, base.y, c1.z), 0)).x;
    float d011 = mipTex.Load(int4(int3(base.x, c1.y, c1.z), 0)).x;
    float d111 = mipTex.Load(int4(int3(c1.x, c1.y, c1.z), 0)).x;
    return lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y), lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
}

bool coneHitOccluded(Texture3D<float4> mip0, Texture3D<float4> mip1, Texture3D<float4> mip2, float3 rayOrigin, float3 rayDir, float travelled, float maxDist, float rays)
{

    int baseRes = int3(textureDimensions(sdf0, 0u)).x;

    int m0Res = int3(textureDimensions(mip0, 0u)).x;
    if (((baseRes <= 0) || (m0Res <= 0)) || (m0Res >= baseRes))
    {
        return false;
    }
    float3 org = itp_params.sdf_origins[0].xyz;
    float3 ext = itp_params.sdf_extents[0].xyz;
    float3 _558 = 1.0f.xxx / rayDir;
    float3 _560 = abs(rayDir);
    bool3 _563 = bool3(_560.x > 1e-08f.xxx.x, _560.y > 1e-08f.xxx.y, _560.z > 1e-08f.xxx.z);
    float3 invD = float3(_563.x ? _558.x : 100000000.0f.xxx.x, _563.y ? _558.y : 100000000.0f.xxx.y, _563.z ? _558.z : 100000000.0f.xxx.z);
    float3 t0s = (org - rayOrigin) * invD;
    float3 t1s = ((org + ext) - rayOrigin) * invD;
    float3 tsm = min(t0s, t1s);
    float3 tbg = max(t0s, t1s);
    float tEnter = max(max(tsm.x, tsm.y), max(tsm.z, 0.0f));
    float tExit = min(min(tbg.x, tbg.y), tbg.z);
    if (tExit <= tEnter)
    {
        return false;
    }
    float sampleT = min(min(travelled, maxDist), tExit);
    if (sampleT <= tEnter)
    {
        return false;
    }
    float3 samplePos = rayOrigin + (rayDir * sampleT);
    float rayAngle = sqrt(12.566371f / max(rays, 1.0f));
    float footprint = sampleT * rayAngle;
    if (footprint <= 0.0f)
    {
        return false;
    }
    float voxel0 = ext.x / float(baseRes);
    float param = footprint;
    float param_1 = voxel0;
    float param_2 = 3.0f;
    float level = coneMipLevelGLSL(param, param_1, param_2);
    float mipD;
    if (level < 0.5f)
    {
        float3 param_3 = samplePos;
        float3 param_4 = org;
        float3 param_5 = ext;
        mipD = sampleSDFCascadeMipGLSL(mip0, param_3, param_4, param_5);
    }
    else
    {
        if (level < 1.5f)
        {

            if (int3(textureDimensions(mip1, 0u)).x >= baseRes)
            {
                return false;
            }
            float3 param_6 = samplePos;
            float3 param_7 = org;
            float3 param_8 = ext;
            mipD = sampleSDFCascadeMipGLSL(mip1, param_6, param_7, param_8);
        }
        else
        {

            if (int3(textureDimensions(mip2, 0u)).x >= baseRes)
            {
                return false;
            }
            float3 param_9 = samplePos;
            float3 param_10 = org;
            float3 param_11 = ext;
            mipD = sampleSDFCascadeMipGLSL(mip2, param_9, param_10, param_11);
        }
    }
    return mipD < (footprint * 0.5f);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint probe_x = input.dispatchThreadID.x;
    uint probe_y = input.dispatchThreadID.y;
    if (itp_params.tile_size == 0u)
    {
        return;
    }
    uint tiles_x = itp_params.sc_params.atlas_size / itp_params.tile_size;
    uint probe_idx = (probe_y * tiles_x) + probe_x;
    if (((itp_params.refresh_stride > 1u) && ((probe_idx % itp_params.refresh_stride) != itp_params.refresh_phase)))
    {
        return;
    }
    uint2 tile_origin = uint2((probe_x * itp_params.tile_size) + (itp_params.tile_size / 2u), (probe_y * itp_params.tile_size) + (itp_params.tile_size / 2u));
    if (((tile_origin.x >= itp_params.sc_params.atlas_size) || (tile_origin.y >= itp_params.sc_params.atlas_size)))
    {
        return;
    }
    uint rays = itp_params.rays_per_probe;
    for (uint r = 0u; r < rays; r++)
    {
        uint hit_idx = (probe_idx * rays) + r;
        ray_hits.Store4(hit_idx * 32 + 0, asuint(0.0f.xxxx));
        ray_hits.Store4(hit_idx * 32 + 16, uint4(4u, 0u, 0u, 0u));
    }
    float depth = depth_atlas[int2(tile_origin)].xxxx.x;
    bool _1014 = (isnan(depth)) || (isinf(depth));
    if (_1014 || (depth <= 0.0f))
    {
        return;
    }
    float2 enc_n = normal_atlas[int2(tile_origin)].xyyy.xy;
    float2 param = enc_n;
    float3 _1033 = octDecode(param);
    float3 probe_normal = _1033;
    uint card_idx = owners.Load(probe_idx * 4);
    if (card_idx >= itp_params.sc_params.max_cards)
    {
        return;
    }
    uint2 param_1 = tile_origin;
    float param_2 = depth;
    SurfaceCacheCard _1063;
    _1063.center = asfloat(cards.Load4(card_idx * 64));
    _1063.extent = asfloat(cards.Load4(card_idx * 64 + 16));
    _1063.axis_direction = cards.Load(card_idx * 64 + 32);
    _1063.resolution = cards.Load(card_idx * 64 + 36);
    _1063.atlas_offset_x = cards.Load(card_idx * 64 + 40);
    _1063.atlas_offset_y = cards.Load(card_idx * 64 + 44);
    _1063.mesh_instance_id = cards.Load(card_idx * 64 + 48);
    _1063.static_mesh_slot = cards.Load(card_idx * 64 + 52);
    _1063.flags = cards.Load(card_idx * 64 + 56);
    _1063.page_card = cards.Load(card_idx * 64 + 60);
    SurfaceCacheCard param_3;
    param_3.center = _1063.center;
    param_3.extent = _1063.extent;
    param_3.axis_direction = _1063.axis_direction;
    param_3.resolution = _1063.resolution;
    param_3.atlas_offset_x = _1063.atlas_offset_x;
    param_3.atlas_offset_y = _1063.atlas_offset_y;
    param_3.mesh_instance_id = _1063.mesh_instance_id;
    param_3.static_mesh_slot = _1063.static_mesh_slot;
    param_3.flags = _1063.flags;
    param_3.page_card = _1063.page_card;
    float3 probe_pos = cardTexelToWorld(param_1, param_2, param_3);
    if (((((any(isnan(probe_pos))) || (any(isinf(probe_pos)))) || (any(isnan(probe_normal)))) || (any(isinf(probe_normal)))))
    {
        return;
    }
    float voxel0 = itp_params.sdf_extents[0].x / max(float(itp_params.sdf_resolutions[0]), 1.0f);
    float hitThreshold = max(voxel0 * 1.25f, 0.3f);
    float minStep = voxel0 * 0.25f;
    float3 ray_origin = probe_pos + (probe_normal * (hitThreshold + 0.05f));
    float advance;
    for (uint r_1 = 0u; r_1 < rays; r_1++)
    {
        uint param_4 = probe_idx;
        uint param_5 = r_1;
        uint param_6 = itp_params.frame_index;
        float2 xi = scTileRayXi(param_4, param_5, param_6);
        float r1 = xi.x;
        float r2 = (xi.y * 2.0f) * 3.1415927f;
        float sin_t = sqrt(r1);
        float cos_t = sqrt(1.0f - r1);
        float3 local_dir = float3(cos_t * cos(r2), cos_t * sin(r2), sin_t);
        bool3 _1194 = (abs(probe_normal.y) < 0.999f).xxx;
        float3 up = float3(_1194.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _1194.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _1194.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
        float3 tangent = normalize(cross(up, probe_normal));
        float3 bitangent = cross(probe_normal, tangent);
        float3 ray_dir = normalize(((tangent * local_dir.x) + (bitangent * local_dir.y)) + (probe_normal * local_dir.z));
        float t = 0.0f;
        bool hit = false;
        bool clearedOriginSkin = false;
        float3 hit_pos = 0.0f.xxx;
        for (uint _step = 0u; _step < 32u; _step++)
        {
            float3 p = ray_origin + (ray_dir * t);
            float3 param_7 = p;
            float d = sampleSDF(param_7);
            if (d < hitThreshold)
            {
                if (!clearedOriginSkin)
                {
                    t += max(abs(d), max(minStep, 0.05f));
                    if (t > itp_params.max_ray_distance)
                    {
                        break;
                    }
                    continue;
                }
                hit = true;
                hit_pos = p;
                break;
            }
            clearedOriginSkin = true;
            if (t > itp_params.max_ray_distance)
            {
                break;
            }
            if (d > (hitThreshold * 4.0f))
            {
                advance = d * 1.2f;
            }
            else
            {
                advance = max(d, minStep);
            }
            t += advance;
        }
        uint hit_idx_1 = (probe_idx * rays) + r_1;
        bool _1309 = (!hit) && clearedOriginSkin;
        bool _1328;
        if (_1309)
        {
            float3 param_8 = ray_origin;
            float3 param_9 = ray_dir;
            float param_10 = t;
            float param_11 = itp_params.max_ray_distance;
            _1328 = coneHitOccluded(sdf0_mip0, sdf0_mip1, sdf0_mip2, param_8, param_9, param_10, param_11, float(rays));
        }
        else
        {
            _1328 = _1309;
        }
        bool coneOccluded = _1328;
        uint3 _1331 = asuint(ray_origin);
        ray_hits.Store(hit_idx_1 * 32 + 20, _1331.x);
        ray_hits.Store(hit_idx_1 * 32 + 24, _1331.y);
        ray_hits.Store(hit_idx_1 * 32 + 28, _1331.z);
        ray_hits.Store(hit_idx_1 * 32 + 12, asuint(t));
        bool _1348;
        if (hit)
        {
            _1348 = t < itp_params.near_distance;
        }
        else
        {
            _1348 = hit;
        }
        if (_1348)
        {
            ray_hits.Store(hit_idx_1 * 32 + 16, 0u);
            ray_hits.Store(hit_idx_1 * 32 + 0, asuint(hit_pos.x));
            ray_hits.Store(hit_idx_1 * 32 + 4, asuint(hit_pos.y));
            ray_hits.Store(hit_idx_1 * 32 + 8, asuint(hit_pos.z));
        }
        else
        {
            if (hit)
            {
                ray_hits.Store(hit_idx_1 * 32 + 16, 1u);
                ray_hits.Store(hit_idx_1 * 32 + 0, asuint(hit_pos.x));
                ray_hits.Store(hit_idx_1 * 32 + 4, asuint(hit_pos.y));
                ray_hits.Store(hit_idx_1 * 32 + 8, asuint(hit_pos.z));
            }
            else
            {
                if (coneOccluded || (!clearedOriginSkin))
                {
                    ray_hits.Store(hit_idx_1 * 32 + 16, 3u);
                    ray_hits.Store(hit_idx_1 * 32 + 0, asuint(0.0f));
                    ray_hits.Store(hit_idx_1 * 32 + 4, asuint(0.0f));
                    ray_hits.Store(hit_idx_1 * 32 + 8, asuint(0.0f));
                }
                else
                {
                    ray_hits.Store(hit_idx_1 * 32 + 16, 2u);
                    ray_hits.Store(hit_idx_1 * 32 + 0, asuint(0.0f));
                    ray_hits.Store(hit_idx_1 * 32 + 4, asuint(0.0f));
                    ray_hits.Store(hit_idx_1 * 32 + 8, asuint(0.0f));
                }
            }
        }
    }
}
