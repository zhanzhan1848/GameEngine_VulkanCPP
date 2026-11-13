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

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

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

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint3 spvTextureSize(Texture3D<float4> Tex, uint Level, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, ret.z, Param);
    return ret;
}

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _233 = n;
    float3 _237 = n;
    float2 _239 = _237.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _233.xy));
    n.x = _239.x;
    n.y = _239.y;
    return normalize(n);
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card)
{
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5f) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5f) / float(card.resolution);
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float3 local_pos;
    if (axis == 0u)
    {
        local_pos.x = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
        local_pos.y = ((u - 0.5f) * card.extent.y) * 2.0f;
        local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
    }
    else
    {
        if (axis == 1u)
        {
            local_pos.y = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
        }
        else
        {
            local_pos.z = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.y = ((v - 0.5f) * card.extent.y) * 2.0f;
        }
    }
    return local_pos + card.center.xyz;
}

uint scTileHash32(inout uint x)
{
    x ^= (x >> 16u);
    x *= 2146121005u;
    x ^= (x >> 15u);
    x *= 2221713035u;
    x ^= (x >> 16u);
    return x;
}

float2 scTileRayXi(uint tileIndex, uint rayIndex, uint frameIndex)
{
    uint seed = ((tileIndex * 2654435769u) ^ (rayIndex * 2246822507u)) ^ (frameIndex * 3266489909u);
    uint param = seed ^ 1757159915u;
    uint _287 = scTileHash32(param);
    uint param_1 = seed ^ 48610963u;
    uint _295 = scTileHash32(param_1);
    return float2(float(_287 >> 8u), float(_295 >> 8u)) * 5.9604644775390625e-08f;
}

float sampleSDFNearest(Texture3D<float4> sdf, float3 uvw)
{
    uint _307_dummy_parameter;
    int3 size = int3(spvTextureSize(sdf, uint(0), _307_dummy_parameter));
    int3 coord = int3(clamp(uvw, 0.0f.xxx, 1.0f.xxx) * float3(size));
    return sdf.Load(int4(clamp(coord, int3(0, 0, 0), size - int3(1, 1, 1)), 0)).x;
}

float sampleSDF(float3 pos)
{
    if (itp_params.sdf_cascade_count.x > 0u)
    {
        float3 uvw = (pos - itp_params.sdf_origins[0].xyz) / itp_params.sdf_extents[0].xyz;
        bool _733 = uvw.x >= 0.0f;
        bool _739;
        if (_733)
        {
            _739 = uvw.x <= 1.0f;
        }
        else
        {
            _739 = _733;
        }
        bool _745;
        if (_739)
        {
            _745 = uvw.y >= 0.0f;
        }
        else
        {
            _745 = _739;
        }
        bool _751;
        if (_745)
        {
            _751 = uvw.y <= 1.0f;
        }
        else
        {
            _751 = _745;
        }
        bool _757;
        if (_751)
        {
            _757 = uvw.z >= 0.0f;
        }
        else
        {
            _757 = _751;
        }
        bool _763;
        if (_757)
        {
            _763 = uvw.z <= 1.0f;
        }
        else
        {
            _763 = _757;
        }
        if (_763)
        {
            float3 param = uvw;
            return sampleSDFNearest(sdf0, param);
        }
    }
    if (itp_params.sdf_cascade_count.x > 1u)
    {
        float3 uvw_1 = (pos - itp_params.sdf_origins[1].xyz) / itp_params.sdf_extents[1].xyz;
        bool _787 = uvw_1.x >= 0.0f;
        bool _793;
        if (_787)
        {
            _793 = uvw_1.x <= 1.0f;
        }
        else
        {
            _793 = _787;
        }
        bool _799;
        if (_793)
        {
            _799 = uvw_1.y >= 0.0f;
        }
        else
        {
            _799 = _793;
        }
        bool _805;
        if (_799)
        {
            _805 = uvw_1.y <= 1.0f;
        }
        else
        {
            _805 = _799;
        }
        bool _811;
        if (_805)
        {
            _811 = uvw_1.z >= 0.0f;
        }
        else
        {
            _811 = _805;
        }
        bool _817;
        if (_811)
        {
            _817 = uvw_1.z <= 1.0f;
        }
        else
        {
            _817 = _811;
        }
        if (_817)
        {
            float3 param_1 = uvw_1;
            return sampleSDFNearest(sdf1, param_1);
        }
    }
    if (itp_params.sdf_cascade_count.x > 2u)
    {
        float3 uvw_2 = (pos - itp_params.sdf_origins[2].xyz) / itp_params.sdf_extents[2].xyz;
        bool _842 = uvw_2.x >= 0.0f;
        bool _848;
        if (_842)
        {
            _848 = uvw_2.x <= 1.0f;
        }
        else
        {
            _848 = _842;
        }
        bool _854;
        if (_848)
        {
            _854 = uvw_2.y >= 0.0f;
        }
        else
        {
            _854 = _848;
        }
        bool _860;
        if (_854)
        {
            _860 = uvw_2.y <= 1.0f;
        }
        else
        {
            _860 = _854;
        }
        bool _866;
        if (_860)
        {
            _866 = uvw_2.z >= 0.0f;
        }
        else
        {
            _866 = _860;
        }
        bool _872;
        if (_866)
        {
            _872 = uvw_2.z <= 1.0f;
        }
        else
        {
            _872 = _866;
        }
        if (_872)
        {
            float3 param_2 = uvw_2;
            return sampleSDFNearest(sdf2, param_2);
        }
    }
    return 1000000.0f;
}

float coneMipLevelGLSL(float footprint, float voxelSize, float maxMip)
{
    return floor(clamp(log2(max(footprint / (voxelSize * 2.0f), 1.0f)), 0.0f, maxMip - 1.0f));
}

float sampleSDFCascadeMipGLSL(Texture3D<float4> mipTex, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _337 = any(bool3(uvw.x < 0.0f.xxx.x, uvw.y < 0.0f.xxx.y, uvw.z < 0.0f.xxx.z));
    bool _344;
    if (!_337)
    {
        _344 = any(bool3(uvw.x > 1.0f.xxx.x, uvw.y > 1.0f.xxx.y, uvw.z > 1.0f.xxx.z));
    }
    else
    {
        _344 = _337;
    }
    if (_344)
    {
        return 10000000000.0f;
    }
    uint _351_dummy_parameter;
    int3 size = int3(spvTextureSize(mipTex, uint(0), _351_dummy_parameter));
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
    uint _513_dummy_parameter;
    int baseRes = int3(spvTextureSize(sdf0, uint(0), _513_dummy_parameter)).x;
    uint _517_dummy_parameter;
    int m0Res = int3(spvTextureSize(mip0, uint(0), _517_dummy_parameter)).x;
    if (((baseRes <= 0) || (m0Res <= 0)) || (m0Res >= baseRes))
    {
        return false;
    }
    float3 org = itp_params.sdf_origins[0].xyz;
    float3 ext = itp_params.sdf_extents[0].xyz;
    float3 _558 = 1.0f.xxx / rayDir;
    float3 _560 = abs(rayDir);
    bool3 _563 = bool3(_560.x > 9.9999999392252902907785028219223e-09f.xxx.x, _560.y > 9.9999999392252902907785028219223e-09f.xxx.y, _560.z > 9.9999999392252902907785028219223e-09f.xxx.z);
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
    float rayAngle = sqrt(12.56637096405029296875f / max(rays, 1.0f));
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
            uint _678_dummy_parameter;
            if (int3(spvTextureSize(mip1, uint(0), _678_dummy_parameter)).x >= baseRes)
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
            uint _694_dummy_parameter;
            if (int3(spvTextureSize(mip2, uint(0), _694_dummy_parameter)).x >= baseRes)
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

void comp_main()
{
    uint probe_x = gl_GlobalInvocationID.x;
    uint probe_y = gl_GlobalInvocationID.y;
    if (itp_params.tile_size == 0u)
    {
        return;
    }
    uint tiles_x = itp_params.sc_params.atlas_size / itp_params.tile_size;
    uint probe_idx = (probe_y * tiles_x) + probe_x;
    bool _914 = itp_params.refresh_stride > 1u;
    bool _925;
    if (_914)
    {
        _925 = (probe_idx % itp_params.refresh_stride) != itp_params.refresh_phase;
    }
    else
    {
        _925 = _914;
    }
    if (_925)
    {
        return;
    }
    uint2 tile_origin = uint2((probe_x * itp_params.tile_size) + (itp_params.tile_size / 2u), (probe_y * itp_params.tile_size) + (itp_params.tile_size / 2u));
    bool _951 = tile_origin.x >= itp_params.sc_params.atlas_size;
    bool _960;
    if (!_951)
    {
        _960 = tile_origin.y >= itp_params.sc_params.atlas_size;
    }
    else
    {
        _960 = _951;
    }
    if (_960)
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
    bool _1008 = isnan(depth);
    bool _1014;
    if (!_1008)
    {
        _1014 = isinf(depth);
    }
    else
    {
        _1014 = _1008;
    }
    if (_1014 || (depth <= 0.0f))
    {
        return;
    }
    float2 enc_n = normal_atlas[int2(tile_origin)].xyyy.xy;
    float2 param = enc_n;
    float3 _1033 = octDecode(param);
    float3 probe_normal = _1033;
    uint card_idx = owners.Load(probe_idx * 4 + 0);
    if (card_idx >= itp_params.sc_params.max_cards)
    {
        return;
    }
    uint2 param_1 = tile_origin;
    float param_2 = depth;
    SurfaceCacheCard _1063;
    _1063.center = asfloat(cards.Load4(card_idx * 64 + 0));
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
    bool _1088 = any(isnan(probe_pos));
    bool _1095;
    if (!_1088)
    {
        _1095 = any(isinf(probe_pos));
    }
    else
    {
        _1095 = _1088;
    }
    bool _1102;
    if (!_1095)
    {
        _1102 = any(isnan(probe_normal));
    }
    else
    {
        _1102 = _1095;
    }
    bool _1109;
    if (!_1102)
    {
        _1109 = any(isinf(probe_normal));
    }
    else
    {
        _1109 = _1102;
    }
    if (_1109)
    {
        return;
    }
    float voxel0 = itp_params.sdf_extents[0].x / max(float(itp_params.sdf_resolutions[0]), 1.0f);
    float hitThreshold = max(voxel0 * 1.25f, 0.300000011920928955078125f);
    float minStep = voxel0 * 0.25f;
    float3 ray_origin = probe_pos + (probe_normal * (hitThreshold + 0.0500000007450580596923828125f));
    float advance;
    for (uint r_1 = 0u; r_1 < rays; r_1++)
    {
        uint param_4 = probe_idx;
        uint param_5 = r_1;
        uint param_6 = itp_params.frame_index;
        float2 xi = scTileRayXi(param_4, param_5, param_6);
        float r1 = xi.x;
        float r2 = (xi.y * 2.0f) * 3.1415927410125732421875f;
        float sin_t = sqrt(r1);
        float cos_t = sqrt(1.0f - r1);
        float3 local_dir = float3(cos_t * cos(r2), cos_t * sin(r2), sin_t);
        bool3 _1194 = (abs(probe_normal.y) < 0.999000012874603271484375f).xxx;
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
                    t += max(abs(d), max(minStep, 0.0500000007450580596923828125f));
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
                advance = d * 1.2000000476837158203125f;
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
            float param_12 = float(rays);
            _1328 = coneHitOccluded(sdf0_mip0, sdf0_mip1, sdf0_mip2, param_8, param_9, param_10, param_11, param_12);
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

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
