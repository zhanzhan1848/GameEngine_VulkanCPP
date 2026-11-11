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

struct IndirectResolveParams
{
    SurfaceCacheParams sc_params;
    uint tile_size;
    uint rays_per_probe;
    float temporal_weight;
    uint frame_index;
    uint lookup_count;
    uint refresh_stride;
    uint refresh_phase;
    float hit_band;
    uint recon_enabled;
    uint card_count;
    uint page_table_enabled;
    float4 tile_sky_color;
    float tile_growth_cap;
    uint ray_lookup;
    float ray_max_distance;
    float _pad_gr;
};

struct SurfaceCacheCardLookup
{
    float4 aabb_min;
    float4 aabb_max;
    uint card_start;
    uint card_count;
    uint _pad0;
    uint _pad1;
};

struct ProbeRayHit
{
    float4 hit_position_dist;
    uint4 hit_type_pad;
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer IndirectResolveParamsBlock : register(b5, space0)
{
    IndirectResolveParams irp_params : packoffset(c0);
};

ByteAddressBuffer lookups : register(t6, space0);
ByteAddressBuffer cards : register(t7, space0);
ByteAddressBuffer pageTable : register(t16, space0);
RWByteAddressBuffer tile_valid : register(u10, space0);
ByteAddressBuffer ray_hits : register(t8, space0);
RWByteAddressBuffer ray_stats : register(u17, space0);
cbuffer GDBlock : register(b4, space0)
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

Texture2D<float4> depth_atlas : register(t11, space0);
Texture2D<float4> normal_atlas : register(t12, space0);
Texture2D<float4> prev_lighting : register(t0, space0);
Texture2D<float4> prev_tile_grid : register(t15, space0);
RWTexture2D<float4> tile_grid : register(u13, space0);
RWTexture2D<float4> tile_aux : register(u14, space0);
Texture2D<float4> prev_indirect_out : register(t9, space0);
RWTexture2D<float4> indirect_out : register(u3, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u1, space0);
Texture2D<float4> sky_cubemap : register(t2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _838 = n;
    float3 _842 = n;
    float2 _844 = _842.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _838.xy));
    n.x = _844.x;
    n.y = _844.y;
    return normalize(n);
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
    uint _891 = scTileHash32(param);
    uint param_1 = seed ^ 48610963u;
    uint _898 = scTileHash32(param_1);
    return float2(float(_891 >> 8u), float(_898 >> 8u)) * 5.9604644775390625e-08f;
}

bool scRayAabbInterval(float3 rayO, float3 invD, float3 bmin, float3 bmax, float t_min, float t_max, inout float t0, inout float t1)
{
    t0 = t_min;
    t1 = t_max;
    for (uint a = 0u; a < 3u; a++)
    {
        if (isinf(invD[a]))
        {
            bool _1758 = rayO[a] < bmin[a];
            bool _1769;
            if (!_1758)
            {
                _1769 = rayO[a] > bmax[a];
            }
            else
            {
                _1769 = _1758;
            }
            if (_1769)
            {
                return false;
            }
        }
        else
        {
            float ta = (bmin[a] - rayO[a]) * invD[a];
            float tb = (bmax[a] - rayO[a]) * invD[a];
            t0 = max(t0, min(ta, tb));
            t1 = min(t1, max(ta, tb));
        }
    }
    return t0 <= t1;
}

SurfaceCacheCard scResolveCardOrigin(inout SurfaceCacheCard card, bool pageTableEnabled, uint pageEntry, uint page_size)
{
    if (pageTableEnabled)
    {
        card.atlas_offset_x = ((pageEntry >> 16u) & 65535u) * page_size;
        card.atlas_offset_y = (pageEntry & 65535u) * page_size;
    }
    return card;
}

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _569;
    if (axis == 0u)
    {
        _569 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _577;
        if (axis == 1u)
        {
            _577 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _577 = float3(0.0f, 0.0f, s);
        }
        _569 = _577;
    }
    return _569;
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

bool scRayCardSurfaceDDA(SurfaceCacheCard card, float3 rayO, float3 rayD, float t_min, float t_max, float tol_band, Texture2D<float4> depthAtlas, out float out_t, out float3 out_pos, out float2 out_atlas_uv, out uint out_reject)
{
    out_t = 0.0f;
    out_pos = 0.0f.xxx;
    out_atlas_uv = 0.0f.xx;
    out_reject = 0u;
    bool _908 = any(isnan(rayO));
    bool _915;
    if (!_908)
    {
        _915 = any(isinf(rayO));
    }
    else
    {
        _915 = _908;
    }
    bool _922;
    if (!_915)
    {
        _922 = any(isnan(rayD));
    }
    else
    {
        _922 = _915;
    }
    bool _929;
    if (!_922)
    {
        _929 = any(isinf(rayD));
    }
    else
    {
        _929 = _922;
    }
    if (_929)
    {
        out_reject = 6u;
        return false;
    }
    SurfaceCacheCard param = card;
    float3 fwd = scCardFaceNormal(param);
    float dn = dot(rayD, fwd);
    if (abs(dn) < 9.9999997473787516355514526367188e-06f)
    {
        out_reject = 1u;
        return false;
    }
    if (dn <= 0.0f)
    {
        out_reject = 2u;
        return false;
    }
    uint axis = card.axis_direction & 255u;
    uint ua = uint(axis == 0u);
    uint va = (axis == 2u) ? 1u : 2u;
    float2 extent = float2(card.extent[ua], card.extent[va]);
    bool _975 = card.resolution == 0u;
    bool _983;
    if (!_975)
    {
        _983 = any(bool2(extent.x <= 0.0f.xx.x, extent.y <= 0.0f.xx.y));
    }
    else
    {
        _983 = _975;
    }
    bool _992;
    if (!_983)
    {
        _992 = any(isnan(card.center));
    }
    else
    {
        _992 = _983;
    }
    bool _1000;
    if (!_992)
    {
        _1000 = any(isinf(card.center));
    }
    else
    {
        _1000 = _992;
    }
    bool _1008;
    if (!_1000)
    {
        _1008 = any(isnan(card.extent));
    }
    else
    {
        _1008 = _1000;
    }
    bool _1016;
    if (!_1008)
    {
        _1016 = any(isinf(card.extent));
    }
    else
    {
        _1016 = _1008;
    }
    if (_1016)
    {
        out_reject = 6u;
        return false;
    }
    float plane = dot(card.center.xyz - rayO, fwd);
    float3 relative = rayO - card.center.xyz;
    float2 uvO = (float2(relative[ua], relative[va]) / (extent * 2.0f)) + 0.5f.xx;
    float2 uvD = float2(rayD[ua], rayD[va]) / (extent * 2.0f);
    float enter = max(t_min, plane / dn);
    float leave = min(t_max, ((plane + (2.0f * card.extent[axis])) + 0.5f) / dn);
    for (uint a = 0u; a < 2u; a++)
    {
        if (abs(uvD[a]) < 9.9999999600419720025001879548654e-13f)
        {
            bool _1095 = uvO[a] < 0.0f;
            bool _1103;
            if (!_1095)
            {
                _1103 = uvO[a] > 1.0f;
            }
            else
            {
                _1103 = _1095;
            }
            if (_1103)
            {
                out_reject = 4u;
                return false;
            }
        }
        else
        {
            float a0 = (0.0f - uvO[a]) / uvD[a];
            float a1 = (1.0f - uvO[a]) / uvD[a];
            enter = max(enter, min(a0, a1));
            leave = min(leave, max(a0, a1));
        }
    }
    if (enter > leave)
    {
        out_reject = 4u;
        return false;
    }
    int res = int(card.resolution);
    int2 cell = int2(clamp(floor((uvO + (uvD * enter)) * float(res)), 0.0f.xx, float(res - 1).xx));
    int2 _step = int2(sign(uvD));
    float2 nextBoundary = 1000000015047466219876688855040.0f.xx;
    float2 increment = 1000000015047466219876688855040.0f.xx;
    for (uint a_1 = 0u; a_1 < 2u; a_1++)
    {
        if (_step[a_1] != 0)
        {
            float boundary = float(cell[a_1] + int(_step[a_1] > 0)) / float(res);
            nextBoundary[a_1] = (boundary - uvO[a_1]) / uvD[a_1];
            increment[a_1] = 1.0f / (float(res) * abs(uvD[a_1]));
        }
    }
    float footprint = length(card.extent.xyz) / float(card.resolution);
    bool validSeen = false;
    for (uint iteration = 0u; iteration < ((2u * card.resolution) + 2u); iteration++)
    {
        bool _1249 = any(bool2(cell.x < int2(0, 0).x, cell.y < int2(0, 0).y));
        bool _1258;
        if (!_1249)
        {
            int2 _1255 = res.xx;
            _1258 = any(bool2(cell.x >= _1255.x, cell.y >= _1255.y));
        }
        else
        {
            _1258 = _1249;
        }
        if (_1258)
        {
            break;
        }
        float exitT = min(leave, min(nextBoundary.x, nextBoundary.y));
        uint2 texel = uint2(cell) + uint2(card.atlas_offset_x, card.atlas_offset_y);
        float depth = depthAtlas.Load(int3(int2(texel), 0)).x;
        bool _1287 = !isnan(depth);
        bool _1293;
        if (_1287)
        {
            _1293 = !isinf(depth);
        }
        else
        {
            _1293 = _1287;
        }
        if (_1293 && (depth >= 0.0f))
        {
            validSeen = true;
            float t = (plane + depth) / dn;
            bool _1308 = t >= (enter - 9.9999997473787516355514526367188e-06f);
            bool _1315;
            if (_1308)
            {
                _1315 = t <= (exitT + 9.9999997473787516355514526367188e-06f);
            }
            else
            {
                _1315 = _1308;
            }
            if ((_1315 && (t >= t_min)) && (t <= t_max))
            {
                uint2 param_1 = texel;
                float param_2 = depth;
                SurfaceCacheCard param_3 = card;
                float3 p = cardTexelToWorld(param_1, param_2, param_3);
                if (length(p - (rayO + (rayD * t))) <= max(footprint * 1.5f, 9.9999997473787516355514526367188e-05f))
                {
                    out_t = t;
                    out_pos = p;
                    out_atlas_uv = float2(texel) + 0.5f.xx;
                    return true;
                }
            }
        }
        if (exitT >= leave)
        {
            break;
        }
        bool crossX = nextBoundary.x <= (exitT + 9.9999999747524270787835121154785e-07f);
        bool crossY = nextBoundary.y <= (exitT + 9.9999999747524270787835121154785e-07f);
        if (crossX)
        {
            cell.x += _step.x;
            nextBoundary.x += increment.x;
        }
        if (crossY)
        {
            cell.y += _step.y;
            nextBoundary.y += increment.y;
        }
        enter = exitT;
    }
    out_reject = validSeen ? 6u : 5u;
    return false;
}

bool worldToCardUVMarginPad(float3 world_pos, SurfaceCacheCard card, inout float2 out_uv, float margin_uv, float pad_m)
{
    float3 local = world_pos - card.center.xyz;
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float depth;
    float u;
    float v;
    if (axis == 0u)
    {
        depth = local.x * ((dir > 0u) ? 1.0f : (-1.0f));
        u = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
    }
    else
    {
        if (axis == 1u)
        {
            depth = local.y * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
        }
        else
        {
            depth = local.z * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        }
    }
    if (depth < 0.0f)
    {
        return false;
    }
    float padU = 0.0f;
    float padV = 0.0f;
    if (pad_m > 0.0f)
    {
        float _412;
        if (axis == 0u)
        {
            _412 = card.extent.y;
        }
        else
        {
            float _420;
            if (axis == 1u)
            {
                _420 = card.extent.x;
            }
            else
            {
                _420 = card.extent.x;
            }
            _412 = _420;
        }
        float eu = _412;
        float _433;
        if (axis == 0u)
        {
            _433 = card.extent.z;
        }
        else
        {
            float _441;
            if (axis == 1u)
            {
                _441 = card.extent.z;
            }
            else
            {
                _441 = card.extent.y;
            }
            _433 = _441;
        }
        float ev = _433;
        float _454;
        if (eu > 9.9999997473787516355514526367188e-05f)
        {
            _454 = pad_m / (eu * 2.0f);
        }
        else
        {
            _454 = 0.0f;
        }
        padU = _454;
        float _465;
        if (ev > 9.9999997473787516355514526367188e-05f)
        {
            _465 = pad_m / (ev * 2.0f);
        }
        else
        {
            _465 = 0.0f;
        }
        padV = _465;
    }
    bool _479 = u < ((-margin_uv) - padU);
    bool _489;
    if (!_479)
    {
        _489 = u >= ((1.0f + margin_uv) + padU);
    }
    else
    {
        _489 = _479;
    }
    bool _499;
    if (!_489)
    {
        _499 = v < ((-margin_uv) - padV);
    }
    else
    {
        _499 = _489;
    }
    bool _509;
    if (!_499)
    {
        _509 = v >= ((1.0f + margin_uv) + padV);
    }
    else
    {
        _509 = _499;
    }
    if (_509)
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999998986721038818359375f);
    v = clamp(v, 0.0f, 0.999998986721038818359375f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

bool worldToCardUVMargin(float3 world_pos, SurfaceCacheCard card, out float2 out_uv, float margin_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = margin_uv;
    float param_4 = 0.0f;
    float2 param_2;
    bool _549 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    out_uv = param_2;
    return _549;
}

bool scRayCardSurfaceT(SurfaceCacheCard card, float3 rayO, float3 rayD, float t_min, float t_max, float tol_band, Texture2D<float4> depthAtlas, out float out_t, out float3 out_pos, out float2 out_atlas_uv, out uint out_reject)
{
    out_t = 0.0f;
    out_pos = 0.0f.xxx;
    out_atlas_uv = 0.0f.xx;
    out_reject = 0u;
    bool _1413 = any(isnan(rayO));
    bool _1420;
    if (!_1413)
    {
        _1420 = any(isinf(rayO));
    }
    else
    {
        _1420 = _1413;
    }
    bool _1427;
    if (!_1420)
    {
        _1427 = any(isnan(rayD));
    }
    else
    {
        _1427 = _1420;
    }
    bool _1434;
    if (!_1427)
    {
        _1434 = any(isinf(rayD));
    }
    else
    {
        _1434 = _1427;
    }
    bool _1442;
    if (!_1434)
    {
        _1442 = any(isnan(card.center));
    }
    else
    {
        _1442 = _1434;
    }
    bool _1450;
    if (!_1442)
    {
        _1450 = any(isinf(card.center));
    }
    else
    {
        _1450 = _1442;
    }
    bool _1458;
    if (!_1450)
    {
        _1458 = any(isnan(card.extent));
    }
    else
    {
        _1458 = _1450;
    }
    bool _1466;
    if (!_1458)
    {
        _1466 = any(isinf(card.extent));
    }
    else
    {
        _1466 = _1458;
    }
    bool _1473;
    if (!_1466)
    {
        _1473 = card.resolution == 0u;
    }
    else
    {
        _1473 = _1466;
    }
    if (_1473)
    {
        out_reject = 6u;
        return false;
    }
    SurfaceCacheCard param = card;
    float3 fwd = scCardFaceNormal(param);
    float dn = dot(rayD, fwd);
    if (abs(dn) < 9.9999997473787516355514526367188e-06f)
    {
        out_reject = 1u;
        return false;
    }
    if (dn <= 0.0f)
    {
        out_reject = 2u;
        return false;
    }
    float plane = dot(card.center.xyz - rayO, fwd);
    float t = clamp(plane / dn, t_min, t_max);
    float footprint = length(card.extent.xyz) / float(card.resolution);
    bool converged = false;
    float2 param_3;
    for (uint iteration = 0u; iteration < 8u; iteration++)
    {
        float3 param_1 = rayO + (rayD * t);
        SurfaceCacheCard param_2 = card;
        float param_4 = 0.0f;
        bool _1540 = worldToCardUVMargin(param_1, param_2, param_3, param_4);
        float2 uv = param_3;
        if (!_1540)
        {
            out_reject = 4u;
            return false;
        }
        int2 texel = int2(clamp(uv, float2(float(card.atlas_offset_x), float(card.atlas_offset_y)), float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
        float depth = depthAtlas.Load(int3(texel, 0)).x;
        bool _1578 = isnan(depth);
        bool _1584;
        if (!_1578)
        {
            _1584 = isinf(depth);
        }
        else
        {
            _1584 = _1578;
        }
        if (_1584 || (depth < 0.0f))
        {
            out_reject = 5u;
            return false;
        }
        float nextT = (plane + depth) / dn;
        if ((nextT < t_min) || (nextT > t_max))
        {
            out_reject = 7u;
            return false;
        }
        if (abs(nextT - t) <= max(9.9999997473787516355514526367188e-05f, footprint * 0.25f))
        {
            t = nextT;
            converged = true;
            break;
        }
        t = nextT;
    }
    if (!converged)
    {
        out_reject = 6u;
        return false;
    }
    float3 param_5 = rayO + (rayD * t);
    SurfaceCacheCard param_6 = card;
    float param_8 = 0.0f;
    float2 param_7;
    bool _1638 = worldToCardUVMargin(param_5, param_6, param_7, param_8);
    float2 uv_1 = param_7;
    if (!_1638)
    {
        out_reject = 4u;
        return false;
    }
    uint2 texel_1 = uint2(clamp(uv_1, float2(float(card.atlas_offset_x), float(card.atlas_offset_y)), float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth_1 = depthAtlas.Load(int3(int2(texel_1), 0)).x;
    bool _1677 = isnan(depth_1);
    bool _1683;
    if (!_1677)
    {
        _1683 = isinf(depth_1);
    }
    else
    {
        _1683 = _1677;
    }
    if (_1683 || (depth_1 < 0.0f))
    {
        out_reject = 5u;
        return false;
    }
    if (abs(depth_1 - ((dn * t) - plane)) > max(9.9999997473787516355514526367188e-05f, footprint * 0.25f))
    {
        out_reject = 6u;
        return false;
    }
    uint2 param_9 = texel_1;
    float param_10 = depth_1;
    SurfaceCacheCard param_11 = card;
    float3 p = cardTexelToWorld(param_9, param_10, param_11);
    if (length(p - (rayO + (rayD * t))) > max(footprint * 1.5f, 9.9999997473787516355514526367188e-05f))
    {
        out_reject = 8u;
        return false;
    }
    out_t = t;
    out_pos = p;
    out_atlas_uv = float2(texel_1) + 0.5f.xx;
    return true;
}

bool findRaySurface(float3 rayO, float3 tileRayDir, inout float bestT, out float2 bestUV, inout uint rejectHist, out uint bestCard)
{
    float tMax = irp_params.ray_max_distance;
    float band = max(irp_params.hit_band, 0.1500000059604644775390625f);
    float3 invD = 1.0f.xxx / tileRayDir;
    bool ptOn2 = irp_params.page_table_enabled != 0u;
    uint ptPage2 = max(irp_params.sc_params.page_size, 1u);
    bool found = false;
    bestCard = 4294967295u;
    bestT = 1000000015047466219876688855040.0f;
    float3 bestPos = 0.0f.xxx;
    bestUV = 0.0f.xx;
    rejectHist = 0u;
    float param_6;
    float param_7;
    SurfaceCacheCard param_8;
    bool surfaceHit;
    float tS;
    float3 pS;
    float2 uvS;
    uint rej;
    float param_18;
    float3 param_19;
    float2 param_20;
    uint param_21;
    float param_28;
    float3 param_29;
    float2 param_30;
    uint param_31;
    for (uint li = 0u; li < irp_params.lookup_count; li++)
    {
        float3 param = rayO;
        float3 param_1 = invD;
        float3 param_2 = asfloat(lookups.Load4(li * 48 + 0)).xyz;
        float3 param_3 = asfloat(lookups.Load4(li * 48 + 16)).xyz;
        float param_4 = 0.0500000007450580596923828125f;
        float param_5 = tMax;
        bool _1907 = scRayAabbInterval(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7);
        float t0 = param_6;
        float t1 = param_7;
        if (!_1907)
        {
            continue;
        }
        bool _1920;
        if (found)
        {
            _1920 = t0 > bestT;
        }
        else
        {
            _1920 = found;
        }
        if (_1920)
        {
            continue;
        }
        for (uint ci = lookups.Load(li * 48 + 32); ci < (lookups.Load(li * 48 + 32) + lookups.Load(li * 48 + 36)); ci++)
        {
            SurfaceCacheCard _1957;
            _1957.center = asfloat(cards.Load4(ci * 64 + 0));
            _1957.extent = asfloat(cards.Load4(ci * 64 + 16));
            _1957.axis_direction = cards.Load(ci * 64 + 32);
            _1957.resolution = cards.Load(ci * 64 + 36);
            _1957.atlas_offset_x = cards.Load(ci * 64 + 40);
            _1957.atlas_offset_y = cards.Load(ci * 64 + 44);
            _1957.mesh_instance_id = cards.Load(ci * 64 + 48);
            _1957.static_mesh_slot = cards.Load(ci * 64 + 52);
            _1957.flags = cards.Load(ci * 64 + 56);
            _1957.page_card = cards.Load(ci * 64 + 60);
            param_8.center = _1957.center;
            param_8.extent = _1957.extent;
            param_8.axis_direction = _1957.axis_direction;
            param_8.resolution = _1957.resolution;
            param_8.atlas_offset_x = _1957.atlas_offset_x;
            param_8.atlas_offset_y = _1957.atlas_offset_y;
            param_8.mesh_instance_id = _1957.mesh_instance_id;
            param_8.static_mesh_slot = _1957.static_mesh_slot;
            param_8.flags = _1957.flags;
            param_8.page_card = _1957.page_card;
            bool param_9 = ptOn2;
            uint param_10 = pageTable.Load(ci * 4 + 0);
            uint param_11 = ptPage2;
            SurfaceCacheCard _1988 = scResolveCardOrigin(param_8, param_9, param_10, param_11);
            SurfaceCacheCard card = _1988;
            if ((irp_params.ray_lookup & 4u) != 0u)
            {
                SurfaceCacheCard param_12 = card;
                float3 param_13 = rayO;
                float3 param_14 = tileRayDir;
                float param_15 = 0.0500000007450580596923828125f;
                float param_16 = tMax;
                float param_17 = band;
                bool _2017 = scRayCardSurfaceDDA(param_12, param_13, param_14, param_15, param_16, param_17, depth_atlas, param_18, param_19, param_20, param_21);
                tS = param_18;
                pS = param_19;
                uvS = param_20;
                rej = param_21;
                surfaceHit = _2017;
            }
            else
            {
                SurfaceCacheCard param_22 = card;
                float3 param_23 = rayO;
                float3 param_24 = tileRayDir;
                float param_25 = 0.0500000007450580596923828125f;
                float param_26 = tMax;
                float param_27 = band;
                bool _2038 = scRayCardSurfaceT(param_22, param_23, param_24, param_25, param_26, param_27, depth_atlas, param_28, param_29, param_30, param_31);
                tS = param_28;
                pS = param_29;
                uvS = param_30;
                rej = param_31;
                surfaceHit = _2038;
            }
            if (surfaceHit)
            {
                if (tS < bestT)
                {
                    bestT = tS;
                    bestPos = pS;
                    bestUV = uvS;
                    bestCard = ci;
                    found = true;
                }
            }
            else
            {
                if ((rej >= 1u) && (rej <= 8u))
                {
                    rejectHist |= (1u << (rej + 3u));
                }
            }
        }
    }
    return found;
}

uint rayTriangle(uint idx)
{
    return 4294967295u;
}

bool findGeometryCache(float3 hitPos, float3 rayO, float3 rayD, uint owner, uint _triangle, out float3 radiance, out float cacheT, inout uint bestCard, inout uint rejectMask)
{
    radiance = 0.0f.xxx;
    cacheT = -1.0f;
    bestCard = 4294967295u;
    rejectMask = 1u;
    float bestKey = 1000000015047466219876688855040.0f;
    bool ptOn = irp_params.page_table_enabled != 0u;
    uint ptPage = max(irp_params.sc_params.page_size, 1u);
    uint cardBegin = 0u;
    uint cardEnd = irp_params.sc_params.max_cards;
    if ((irp_params.ray_lookup & 2048u) != 0u)
    {
        bool direct = false;
        if ((irp_params.ray_lookup & 4096u) != 0u)
        {
            if (owner >= irp_params.lookup_count)
            {
                return false;
            }
            uint first = lookups.Load(owner * 48 + 40);
            uint last = lookups.Load(owner * 48 + 44);
            if ((first == 0u) && (last == 0u))
            {
                return false;
            }
            bool _2130 = first < last;
            bool _2137;
            if (_2130)
            {
                _2137 = last <= irp_params.sc_params.max_cards;
            }
            else
            {
                _2137 = _2130;
            }
            bool _2145;
            if (_2137)
            {
                _2145 = cards.Load(first * 64 + 48) == owner;
            }
            else
            {
                _2145 = _2137;
            }
            bool _2154;
            if (_2145)
            {
                _2154 = cards.Load((last - 1u) * 64 + 48) == owner;
            }
            else
            {
                _2154 = _2145;
            }
            if (_2154)
            {
                cardBegin = first;
                cardEnd = last;
                direct = true;
            }
        }
        if (!direct)
        {
            cardBegin = cardEnd;
            cardEnd = 0u;
            for (uint li = 0u; li < irp_params.lookup_count; li++)
            {
                uint first_1 = lookups.Load(li * 48 + 32);
                uint count = lookups.Load(li * 48 + 36);
                if (count == 0u)
                {
                    continue;
                }
                bool _2190 = first_1 >= irp_params.sc_params.max_cards;
                bool _2200;
                if (!_2190)
                {
                    _2200 = count > (irp_params.sc_params.max_cards - first_1);
                }
                else
                {
                    _2200 = _2190;
                }
                if (_2200)
                {
                    cardBegin = 0u;
                    cardEnd = irp_params.sc_params.max_cards;
                    break;
                }
                if (cards.Load(first_1 * 64 + 48) == owner)
                {
                    cardBegin = min(cardBegin, first_1);
                    cardEnd = max(cardEnd, (first_1 + count));
                }
            }
        }
        if (cardBegin >= cardEnd)
        {
            return false;
        }
    }
    SurfaceCacheCard param;
    float2 param_6;
    for (uint ci = cardBegin; ci < cardEnd; ci++)
    {
        bool _2243 = cards.Load(ci * 64 + 48) != owner;
        bool _2251;
        if (!_2243)
        {
            _2251 = cards.Load(ci * 64 + 36) == 0u;
        }
        else
        {
            _2251 = _2243;
        }
        if (_2251)
        {
            continue;
        }
        rejectMask &= 4294967294u;
        SurfaceCacheCard _2263;
        _2263.center = asfloat(cards.Load4(ci * 64 + 0));
        _2263.extent = asfloat(cards.Load4(ci * 64 + 16));
        _2263.axis_direction = cards.Load(ci * 64 + 32);
        _2263.resolution = cards.Load(ci * 64 + 36);
        _2263.atlas_offset_x = cards.Load(ci * 64 + 40);
        _2263.atlas_offset_y = cards.Load(ci * 64 + 44);
        _2263.mesh_instance_id = cards.Load(ci * 64 + 48);
        _2263.static_mesh_slot = cards.Load(ci * 64 + 52);
        _2263.flags = cards.Load(ci * 64 + 56);
        _2263.page_card = cards.Load(ci * 64 + 60);
        param.center = _2263.center;
        param.extent = _2263.extent;
        param.axis_direction = _2263.axis_direction;
        param.resolution = _2263.resolution;
        param.atlas_offset_x = _2263.atlas_offset_x;
        param.atlas_offset_y = _2263.atlas_offset_y;
        param.mesh_instance_id = _2263.mesh_instance_id;
        param.static_mesh_slot = _2263.static_mesh_slot;
        param.flags = _2263.flags;
        param.page_card = _2263.page_card;
        bool param_1 = ptOn;
        uint param_2 = pageTable.Load(ci * 4 + 0);
        uint param_3 = ptPage;
        SurfaceCacheCard _2291 = scResolveCardOrigin(param, param_1, param_2, param_3);
        SurfaceCacheCard card = _2291;
        float3 param_4 = hitPos;
        SurfaceCacheCard param_5 = card;
        float param_7 = 0.0f;
        float param_8 = 0.0f;
        bool _2300 = worldToCardUVMarginPad(param_4, param_5, param_6, param_7, param_8);
        float2 uv = param_6;
        bool _2302 = !_2300;
        bool _2313;
        if (!_2302)
        {
            SurfaceCacheCard param_9 = card;
            _2313 = dot(scCardFaceNormal(param_9), -rayD) >= 0.0f;
        }
        else
        {
            _2313 = _2302;
        }
        if (_2313)
        {
            rejectMask |= 2u;
            continue;
        }
    }
    return bestCard != 4294967295u;
}

bool scQueryCardCandidateDbg(SurfaceCacheCard card, float3 hitPos, float3 surfaceFacing, float tolFloor, float qpadM, Texture2D<float4> depthAtlas, Texture2D<float4> lightingAtlas, float3 hitNormalHint, out float3 outRadiance, out float outKey, out uint outReject)
{
    outRadiance = 0.0f.xxx;
    outKey = 1000000015047466219876688855040.0f;
    outReject = 0u;
    float3 param = hitPos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 1.0f / float(max(card.resolution, 1u));
    float param_4 = qpadM;
    float2 param_2;
    bool _605 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    float2 atlasUV = param_2;
    if (!_605)
    {
        outReject = 1u;
        return false;
    }
    SurfaceCacheCard param_5 = card;
    if (dot(scCardFaceNormal(param_5), surfaceFacing) >= 0.0f)
    {
        outReject = 2u;
        return false;
    }
    float hintPenalty = 0.0f;
    if (dot(hitNormalHint, hitNormalHint) > 0.25f)
    {
        SurfaceCacheCard param_6 = card;
        float d = dot(scCardFaceNormal(param_6), hitNormalHint);
        if (d > (-0.5f))
        {
            outReject = 7u;
        }
        hintPenalty = max(d + 1.0f, 0.0f);
    }
    uint2 texel = uint2(clamp(atlasUV, 0.0f.xx, float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth = depthAtlas.Load(int3(int2(texel), 0)).x;
    bool _671 = isnan(depth);
    bool _677;
    if (!_671)
    {
        _677 = isinf(depth);
    }
    else
    {
        _677 = _671;
    }
    if (_677 || (depth < 0.0f))
    {
        outReject = 3u;
        return false;
    }
    uint2 param_7 = texel;
    float param_8 = depth;
    SurfaceCacheCard param_9 = card;
    float3 captured = cardTexelToWorld(param_7, param_8, param_9);
    float3 delta = captured - hitPos;
    SurfaceCacheCard param_10 = card;
    float3 n = scCardFaceNormal(param_10);
    float along = dot(delta, n);
    float3 perpV = delta - (n * along);
    float perp = length(perpV);
    float footprint = length(card.extent.xyz) / float(max(card.resolution, 1u));
    float tolAlong = tolFloor + footprint;
    float tolPerp = max(footprint * 1.5f, tolFloor);
    if (abs(along) > tolAlong)
    {
        outReject = 4u;
        return false;
    }
    if (perp > tolPerp)
    {
        outReject = 5u;
        return false;
    }
    float3 c = lightingAtlas.Load(int3(int2(texel), 0)).xyz;
    bool _758 = any(isnan(c));
    bool _765;
    if (!_758)
    {
        _765 = any(isinf(c));
    }
    else
    {
        _765 = _758;
    }
    if (_765)
    {
        outReject = 6u;
        return false;
    }
    outRadiance = c;
    outKey = (perp + (0.5f * abs(along))) + (hintPenalty * ((perp + abs(along)) + max(tolFloor, 0.0500000007450580596923828125f)));
    return true;
}

bool scQueryCardCandidate(SurfaceCacheCard card, float3 hitPos, float3 surfaceFacing, float tolFloor, Texture2D<float4> depthAtlas, Texture2D<float4> lightingAtlas, float3 hitNormalHint, out float3 outRadiance, out float outKey)
{
    SurfaceCacheCard param = card;
    float3 param_1 = hitPos;
    float3 param_2 = surfaceFacing;
    float param_3 = tolFloor;
    float param_4 = 0.0f;
    float3 param_5 = hitNormalHint;
    float3 param_6;
    float param_7;
    uint param_8;
    bool _804 = scQueryCardCandidateDbg(param, param_1, param_2, param_3, param_4, depthAtlas, lightingAtlas, param_5, param_6, param_7, param_8);
    outRadiance = param_6;
    outKey = param_7;
    uint rejectDummy = param_8;
    return _804;
}

float3 EvaluateSkyBase(float3 dir)
{
    float up = clamp(dir.y, 0.0f, 1.0f);
    float3 horizonCol = float3(4.400000095367431640625f, 4.19999980926513671875f, 3.7999999523162841796875f);
    float3 zenithCol = float3(5.5f, 5.7600002288818359375f, 6.0f);
    return lerp(horizonCol, zenithCol, pow(up, 0.449999988079071044921875f).xxx);
}

void comp_main()
{
    uint probe_x = gl_GlobalInvocationID.x;
    uint probe_y = gl_GlobalInvocationID.y;
    uint tiles_x = irp_params.sc_params.atlas_size / irp_params.tile_size;
    uint probe_idx = (probe_y * tiles_x) + probe_x;
    uint rays = irp_params.rays_per_probe;
    uint tile_size = irp_params.tile_size;
    uint2 tile_origin = uint2(probe_x * tile_size, probe_y * tile_size);
    bool _2365 = tile_origin.x >= irp_params.sc_params.atlas_size;
    bool _2374;
    if (!_2365)
    {
        _2374 = tile_origin.y >= irp_params.sc_params.atlas_size;
    }
    else
    {
        _2374 = _2365;
    }
    if (_2374)
    {
        return;
    }
    bool _2381 = irp_params.refresh_stride <= 1u;
    bool _2392;
    if (!_2381)
    {
        _2392 = (probe_idx % irp_params.refresh_stride) == irp_params.refresh_phase;
    }
    else
    {
        _2392 = _2381;
    }
    bool refreshed = _2392;
    uint tvWord = probe_idx >> 5u;
    uint tvBit = 1u << (probe_idx & 31u);
    uint _2408;
    tile_valid.InterlockedOr(tvWord * 4 + 0, 0u, _2408);
    uint validityWord = _2408;
    bool tileWasValid = (validityWord & tvBit) != 0u;
    float2 enc_n = normal_atlas.Load(int3(int2(tile_origin + (tile_size / 2u).xx), 0)).xy;
    float2 param = enc_n;
    float3 _2428 = octDecode(param);
    float3 probe_normal = _2428;
    float3 tileRayDir = float3(0.0f, -1.0f, 0.0f);
    float3 indirect = 0.0f.xxx;
    uint currentRayCount = 0u;
    uint measurementCount = 0u;
    if (refreshed)
    {
        float param_6;
        float2 param_7;
        uint param_8;
        uint param_9;
        float3 param_16;
        float param_17;
        uint param_18;
        uint param_19;
        float3 param_26;
        float param_27;
        uint param_28;
        uint param_29;
        float param_32;
        float2 param_33;
        uint param_34;
        uint param_35;
        SurfaceCacheCard param_36;
        float3 param_45;
        float param_46;
        for (uint r = 0u; r < rays; r++)
        {
            uint hit_idx = (probe_idx * rays) + r;
            float3 hit_pos = asfloat(ray_hits.Load4(hit_idx * 32 + 0)).xyz;
            uint hit_identity = ray_hits.Load(hit_idx * 32 + 16);
            uint hit_type = hit_identity & 7u;
            float3 radiance = 0.0f.xxx;
            if (hit_type >= 4u)
            {
                continue;
            }
            currentRayCount++;
            if (hit_type == 3u)
            {
                measurementCount++;
                continue;
            }
            uint param_1 = probe_idx;
            uint param_2 = r;
            uint param_3 = irp_params.frame_index;
            float2 xi = scTileRayXi(param_1, param_2, param_3);
            float r1 = xi.x;
            float r2 = (xi.y * 2.0f) * 3.1415927410125732421875f;
            float sin_t = sqrt(r1);
            float cos_t = sqrt(1.0f - r1);
            float3 local_dir = float3(cos_t * cos(r2), cos_t * sin(r2), sin_t);
            bool3 _2529 = (abs(probe_normal.y) < 0.999000012874603271484375f).xxx;
            float3 up = float3(_2529.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _2529.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _2529.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
            float3 tangent = normalize(cross(up, probe_normal));
            float3 bitangent = cross(probe_normal, tangent);
            tileRayDir = normalize(((tangent * local_dir.x) + (bitangent * local_dir.y)) + (probe_normal * local_dir.z));
            bool _2558 = (irp_params.ray_lookup & 2u) != 0u;
            bool _2565;
            if (_2558)
            {
                _2565 = (hit_idx % 37u) == 0u;
            }
            else
            {
                _2565 = _2558;
            }
            bool _2572;
            if (_2565)
            {
                _2572 = (hit_idx / 37u) < 16384u;
            }
            else
            {
                _2572 = _2565;
            }
            if (_2572)
            {
                float3 origin = asfloat(ray_hits.Load4(hit_idx * 32 + 16).yzw);
                float3 param_4 = origin;
                float3 param_5 = tileRayDir;
                bool _2595 = findRaySurface(param_4, param_5, param_6, param_7, param_8, param_9);
                float bestT = param_6;
                float2 bestUV = param_7;
                uint rejects = param_8;
                uint bestCard = param_9;
                bool found = _2595;
                if ((irp_params.ray_lookup & 8u) != 0u)
                {
                    rejects = 0u;
                    bool _2610 = (hit_type == 0u) || (hit_type == 1u);
                    bool _2616;
                    if (_2610)
                    {
                        _2616 = (hit_identity >> 3u) != 0u;
                    }
                    else
                    {
                        _2616 = _2610;
                    }
                    bool _2643;
                    if (_2616)
                    {
                        uint param_10 = hit_idx;
                        float3 param_11 = hit_pos;
                        float3 param_12 = origin;
                        float3 param_13 = tileRayDir;
                        uint param_14 = (hit_identity >> 3u) - 1u;
                        uint param_15 = rayTriangle(param_10);
                        bool _2638 = findGeometryCache(param_11, param_12, param_13, param_14, param_15, param_16, param_17, param_18, param_19);
                        float3 cacheRadiance = param_16;
                        bestT = param_17;
                        bestCard = param_18;
                        rejects = param_19;
                        _2643 = _2638;
                    }
                    else
                    {
                        _2643 = _2616;
                    }
                    found = _2643;
                }
                uint at = 16u + ((hit_idx / 37u) * 16u);
                ray_stats.Store((at + 0u) * 4 + 0, asuint(origin.x));
                ray_stats.Store((at + 1u) * 4 + 0, asuint(origin.y));
                ray_stats.Store((at + 2u) * 4 + 0, asuint(origin.z));
                ray_stats.Store((at + 3u) * 4 + 0, asuint(0.0500000007450580596923828125f));
                ray_stats.Store((at + 4u) * 4 + 0, asuint(tileRayDir.x));
                ray_stats.Store((at + 5u) * 4 + 0, asuint(tileRayDir.y));
                ray_stats.Store((at + 6u) * 4 + 0, asuint(tileRayDir.z));
                ray_stats.Store((at + 7u) * 4 + 0, asuint(irp_params.ray_max_distance));
                ray_stats.Store((at + 8u) * 4 + 0, asuint(asfloat(ray_hits.Load(hit_idx * 32 + 12))));
                ray_stats.Store((at + 9u) * 4 + 0, asuint(found ? bestT : (-1.0f)));
                ray_stats.Store((at + 10u) * 4 + 0, hit_idx);
                ray_stats.Store((at + 11u) * 4 + 0, hit_type);
                ray_stats.Store((at + 12u) * 4 + 0, irp_params.frame_index + 1u);
                ray_stats.Store((at + 13u) * 4 + 0, uint(found));
                ray_stats.Store((at + 14u) * 4 + 0, rejects);
                ray_stats.Store((at + 15u) * 4 + 0, bestCard);
            }
            if ((hit_type == 0u) || (hit_type == 1u))
            {
                if ((irp_params.ray_lookup & 8u) != 0u)
                {
                    if ((hit_identity >> 3u) == 0u)
                    {
                        continue;
                    }
                    float3 origin_1 = asfloat(ray_hits.Load4(hit_idx * 32 + 16).yzw);
                    uint param_20 = hit_idx;
                    float3 param_21 = hit_pos;
                    float3 param_22 = origin_1;
                    float3 param_23 = tileRayDir;
                    uint param_24 = (hit_identity >> 3u) - 1u;
                    uint param_25 = rayTriangle(param_20);
                    bool _2792 = findGeometryCache(param_21, param_22, param_23, param_24, param_25, param_26, param_27, param_28, param_29);
                    radiance = param_26;
                    float cacheT = param_27;
                    uint bestCard_1 = param_28;
                    uint rejectMask = param_29;
                    if (!_2792)
                    {
                        continue;
                    }
                    indirect += radiance;
                    measurementCount++;
                    continue;
                }
                if ((irp_params.ray_lookup & 1u) != 0u)
                {
                    float band = max(irp_params.hit_band, 0.1500000059604644775390625f);
                    float sdfDist = asfloat(ray_hits.Load(hit_idx * 32 + 12));
                    float3 rayO = hit_pos - (tileRayDir * sdfDist);
                    float3 param_30 = rayO;
                    float3 param_31 = tileRayDir;
                    bool _2840 = findRaySurface(param_30, param_31, param_32, param_33, param_34, param_35);
                    float bestT_1 = param_32;
                    float2 bestUV_1 = param_33;
                    uint rejectHist = param_34;
                    uint bestCard_2 = param_35;
                    bool found_1 = _2840;
                    uint _2846;
                    ray_stats.InterlockedAdd(0, 1u, _2846);
                    if (rejectHist != 0u)
                    {
                        for (uint b = 0u; b < 8u; b++)
                        {
                            if ((rejectHist & (1u << (b + 4u))) != 0u)
                            {
                                uint _2870;
                                ray_stats.InterlockedAdd((b + 6u) * 4 + 0, 1u, _2870);
                            }
                        }
                    }
                    if (found_1)
                    {
                        float delta = abs(bestT_1 - sdfDist);
                        if (delta <= 0.0500000007450580596923828125f)
                        {
                            uint _2886;
                            ray_stats.InterlockedAdd(8, 1u, _2886);
                        }
                        else
                        {
                            if (delta <= 0.1500000059604644775390625f)
                            {
                                uint _2893;
                                ray_stats.InterlockedAdd(12, 1u, _2893);
                            }
                            else
                            {
                                if (delta <= 0.449999988079071044921875f)
                                {
                                    uint _2900;
                                    ray_stats.InterlockedAdd(16, 1u, _2900);
                                }
                                else
                                {
                                    uint _2903;
                                    ray_stats.InterlockedAdd(20, 1u, _2903);
                                }
                            }
                        }
                        uint _2905;
                        ray_stats.InterlockedAdd(4, 1u, _2905);
                        uint2 texel = uint2(clamp(bestUV_1, 0.0f.xx, float(irp_params.sc_params.atlas_size - 1u).xx));
                        float3 cand = prev_lighting.Load(int3(int2(texel), 0)).xyz;
                        bool _2924 = any(isnan(cand));
                        bool _2931;
                        if (!_2924)
                        {
                            _2931 = any(isinf(cand));
                        }
                        else
                        {
                            _2931 = _2924;
                        }
                        if (_2931)
                        {
                            continue;
                        }
                        radiance = cand;
                        measurementCount++;
                        indirect += cand;
                        continue;
                    }
                    continue;
                }
                bool found_2 = false;
                float bestKey = 1000000015047466219876688855040.0f;
                bool ptOn = irp_params.page_table_enabled != 0u;
                uint ptPage = max(irp_params.sc_params.page_size, 1u);
                for (uint li = 0u; (li < irp_params.lookup_count) && (!found_2); li++)
                {
                    float3 mn = asfloat(lookups.Load4(li * 48 + 0)).xyz;
                    float3 mx = asfloat(lookups.Load4(li * 48 + 16)).xyz;
                    bool _2980 = hit_pos.x < mn.x;
                    bool _2989;
                    if (!_2980)
                    {
                        _2989 = hit_pos.x > mx.x;
                    }
                    else
                    {
                        _2989 = _2980;
                    }
                    bool _2998;
                    if (!_2989)
                    {
                        _2998 = hit_pos.y < mn.y;
                    }
                    else
                    {
                        _2998 = _2989;
                    }
                    bool _3007;
                    if (!_2998)
                    {
                        _3007 = hit_pos.y > mx.y;
                    }
                    else
                    {
                        _3007 = _2998;
                    }
                    bool _3016;
                    if (!_3007)
                    {
                        _3016 = hit_pos.z < mn.z;
                    }
                    else
                    {
                        _3016 = _3007;
                    }
                    bool _3025;
                    if (!_3016)
                    {
                        _3025 = hit_pos.z > mx.z;
                    }
                    else
                    {
                        _3025 = _3016;
                    }
                    if (_3025)
                    {
                        continue;
                    }
                    for (uint ci = lookups.Load(li * 48 + 32); ci < (lookups.Load(li * 48 + 32) + lookups.Load(li * 48 + 36)); ci++)
                    {
                        SurfaceCacheCard _3052;
                        _3052.center = asfloat(cards.Load4(ci * 64 + 0));
                        _3052.extent = asfloat(cards.Load4(ci * 64 + 16));
                        _3052.axis_direction = cards.Load(ci * 64 + 32);
                        _3052.resolution = cards.Load(ci * 64 + 36);
                        _3052.atlas_offset_x = cards.Load(ci * 64 + 40);
                        _3052.atlas_offset_y = cards.Load(ci * 64 + 44);
                        _3052.mesh_instance_id = cards.Load(ci * 64 + 48);
                        _3052.static_mesh_slot = cards.Load(ci * 64 + 52);
                        _3052.flags = cards.Load(ci * 64 + 56);
                        _3052.page_card = cards.Load(ci * 64 + 60);
                        param_36.center = _3052.center;
                        param_36.extent = _3052.extent;
                        param_36.axis_direction = _3052.axis_direction;
                        param_36.resolution = _3052.resolution;
                        param_36.atlas_offset_x = _3052.atlas_offset_x;
                        param_36.atlas_offset_y = _3052.atlas_offset_y;
                        param_36.mesh_instance_id = _3052.mesh_instance_id;
                        param_36.static_mesh_slot = _3052.static_mesh_slot;
                        param_36.flags = _3052.flags;
                        param_36.page_card = _3052.page_card;
                        bool param_37 = ptOn;
                        uint param_38 = pageTable.Load(ci * 4 + 0);
                        uint param_39 = ptPage;
                        SurfaceCacheCard _3080 = scResolveCardOrigin(param_36, param_37, param_38, param_39);
                        SurfaceCacheCard card = _3080;
                        SurfaceCacheCard param_40 = card;
                        float3 param_41 = hit_pos;
                        float3 param_42 = -tileRayDir;
                        float param_43 = max(irp_params.hit_band, 0.1500000059604644775390625f);
                        float3 param_44 = 0.0f.xxx;
                        bool _3097 = scQueryCardCandidate(param_40, param_41, param_42, param_43, depth_atlas, prev_lighting, param_44, param_45, param_46);
                        float3 cand_1 = param_45;
                        float key = param_46;
                        if (_3097)
                        {
                            if (key < bestKey)
                            {
                                bestKey = key;
                                radiance = cand_1;
                                found_2 = true;
                            }
                        }
                    }
                }
                if (!found_2)
                {
                    continue;
                }
            }
            else
            {
                if (irp_params.tile_sky_color.w > 0.5f)
                {
                    float3 param_47 = tileRayDir;
                    radiance = EvaluateSkyBase(param_47) * irp_params.tile_sky_color.xyz;
                }
                else
                {
                    radiance = irp_params.tile_sky_color.xyz;
                }
            }
            indirect += radiance;
            measurementCount++;
        }
    }
    uint minMeasurements = min(rays, 4u);
    bool currentEstimateValid = (refreshed && (currentRayCount == rays)) && (measurementCount >= minMeasurements);
    if (currentEstimateValid)
    {
        indirect /= float(max(rays, 1u)).xxx;
    }
    else
    {
        indirect = 0.0f.xxx;
    }
    if (currentEstimateValid)
    {
        uint _3172;
        tile_valid.InterlockedOr(tvWord * 4 + 0, tvBit, _3172);
    }
    float tw = clamp(irp_params.temporal_weight, 0.0f, 0.949999988079071044921875f);
    bool hasHistory = irp_params.frame_index >= 2u;
    if ((((irp_params.tile_growth_cap > 0.0f) && currentEstimateValid) && tileWasValid) && hasHistory)
    {
        float3 prevCap = prev_tile_grid.Load(int3(int2(int(probe_x), int(probe_y)), 0)).xyz;
        if (all(bool3(prevCap.x >= 0.0f.xxx.x, prevCap.y >= 0.0f.xxx.y, prevCap.z >= 0.0f.xxx.z)))
        {
            float3 cap = (prevCap * irp_params.tile_growth_cap) + 0.0199999995529651641845703125f.xxx;
            indirect = min(indirect, prevCap + cap);
        }
    }
    if (irp_params.recon_enabled != 0u)
    {
        float3 tileValue = indirect;
        float tileValid;
        if (!currentEstimateValid)
        {
            tileValid = float(tileWasValid && hasHistory);
            float3 _3242;
            if (tileWasValid && hasHistory)
            {
                _3242 = prev_tile_grid.Load(int3(int2(int(probe_x), int(probe_y)), 0)).xyz;
            }
            else
            {
                _3242 = 0.0f.xxx;
            }
            float3 prev = _3242;
            tileValue = prev;
        }
        else
        {
            if (hasHistory && (tw > 0.0f))
            {
                float3 prev_1 = prev_tile_grid.Load(int3(int2(int(probe_x), int(probe_y)), 0)).xyz;
                tileValue = lerp(indirect, prev_1, tw.xxx);
                tileValid = 1.0f;
            }
            else
            {
                tileValid = 1.0f;
            }
        }
        tile_grid[int2(int(probe_x), int(probe_y))] = float4(tileValue, tileValid);
        uint2 centerTexel = tile_origin + (tile_size / 2u).xx;
        uint foundCard = 4095u;
        SurfaceCacheCard c;
        for (uint ci_1 = 0u; ci_1 < irp_params.card_count; ci_1++)
        {
            SurfaceCacheCard _3315;
            _3315.center = asfloat(cards.Load4(ci_1 * 64 + 0));
            _3315.extent = asfloat(cards.Load4(ci_1 * 64 + 16));
            _3315.axis_direction = cards.Load(ci_1 * 64 + 32);
            _3315.resolution = cards.Load(ci_1 * 64 + 36);
            _3315.atlas_offset_x = cards.Load(ci_1 * 64 + 40);
            _3315.atlas_offset_y = cards.Load(ci_1 * 64 + 44);
            _3315.mesh_instance_id = cards.Load(ci_1 * 64 + 48);
            _3315.static_mesh_slot = cards.Load(ci_1 * 64 + 52);
            _3315.flags = cards.Load(ci_1 * 64 + 56);
            _3315.page_card = cards.Load(ci_1 * 64 + 60);
            c.center = _3315.center;
            c.extent = _3315.extent;
            c.axis_direction = _3315.axis_direction;
            c.resolution = _3315.resolution;
            c.atlas_offset_x = _3315.atlas_offset_x;
            c.atlas_offset_y = _3315.atlas_offset_y;
            c.mesh_instance_id = _3315.mesh_instance_id;
            c.static_mesh_slot = _3315.static_mesh_slot;
            c.flags = _3315.flags;
            c.page_card = _3315.page_card;
            if (c.resolution == 0u)
            {
                continue;
            }
            bool _3346 = centerTexel.x >= c.atlas_offset_x;
            bool _3357;
            if (_3346)
            {
                _3357 = centerTexel.x < (c.atlas_offset_x + c.resolution);
            }
            else
            {
                _3357 = _3346;
            }
            bool _3365;
            if (_3357)
            {
                _3365 = centerTexel.y >= c.atlas_offset_y;
            }
            else
            {
                _3365 = _3357;
            }
            bool _3376;
            if (_3365)
            {
                _3376 = centerTexel.y < (c.atlas_offset_y + c.resolution);
            }
            else
            {
                _3376 = _3365;
            }
            if (_3376)
            {
                foundCard = ci_1;
                break;
            }
        }
        float centerDepth = depth_atlas.Load(int3(int2(centerTexel), 0)).x;
        tile_aux[int2(int(probe_x), int(probe_y))] = float4(enc_n.x, enc_n.y, centerDepth, float(foundCard));
        return;
    }
    for (uint dy = 0u; dy < tile_size; dy++)
    {
        for (uint dx = 0u; dx < tile_size; dx++)
        {
            uint2 texel_1 = tile_origin + uint2(dx, dy);
            bool _3433 = texel_1.x >= irp_params.sc_params.atlas_size;
            bool _3442;
            if (!_3433)
            {
                _3442 = texel_1.y >= irp_params.sc_params.atlas_size;
            }
            else
            {
                _3442 = _3433;
            }
            if (_3442)
            {
                continue;
            }
            if (!currentEstimateValid)
            {
                if (tileWasValid && hasHistory)
                {
                    float3 prev_2 = prev_indirect_out.Load(int3(int2(texel_1), 0)).xyz;
                    indirect_out[int2(texel_1)] = float4(prev_2, 1.0f);
                }
                else
                {
                    indirect_out[int2(texel_1)] = 0.0f.xxxx;
                }
                continue;
            }
            float3 albedo = albedo_atlas[int2(texel_1)].xyz;
            float3 current = indirect * albedo;
            if (hasHistory && (tw > 0.0f))
            {
                float3 prev_3 = prev_indirect_out.Load(int3(int2(texel_1), 0)).xyz;
                current = lerp(current, prev_3, tw.xxx);
            }
            indirect_out[int2(texel_1)] = float4(current, 1.0f);
        }
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
