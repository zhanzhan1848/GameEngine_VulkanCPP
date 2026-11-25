// Vulkan reference: Lumen/WRCTraceFinalize.comp (wrc_trace_finalize).
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

struct SCGridParams
{
    float3 grid_min;
    float3 grid_max;
    uint3 dims;
    float cell_size;
    uint card_count;
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

cbuffer WRCVolumeDataBlock : register(b2, space0)
{
    float4 volume_ProbeOrigin : packoffset(c0);
    uint4 volume_ProbeCounts : packoffset(c1);
    uint4 volume_ProbeMeta : packoffset(c2);
    float4 volume_TraceParams : packoffset(c3);
    float4 volume_SdfOrigins[3] : packoffset(c4);
    float4 volume_SdfVoxelSizes[3] : packoffset(c7);
    float4 volume_SdfExtents[3] : packoffset(c10);
    uint4 volume_SdfResolutionsAndCount : packoffset(c13);
    float4 volume_LightDirection : packoffset(c14);
    float4 volume_LightColor : packoffset(c15);
    float4 volume_SkyColor : packoffset(c16);
    uint4 volume_SCParams : packoffset(c17);
    float4 volume_SCQueryParams : packoffset(c18);
};

ByteAddressBuffer scgo : register(t8, space0);
ByteAddressBuffer pul : register(t5, space0);
ByteAddressBuffer hd : register(t3, space0);
RWByteAddressBuffer dbg : register(u10, space0);
ByteAddressBuffer scgc : register(t9, space0);
ByteAddressBuffer cd : register(t7, space0);
RWByteAddressBuffer wrcCardFailBuf : register(u12, space0);
RWByteAddressBuffer bins : register(u11, space0);
RWByteAddressBuffer rd : register(u4, space0);
ByteAddressBuffer cl : register(t6, space0);
Texture2D<float4> lighting_atlas : register(t0, space0);
Texture2D<float4> depth_atlas : register(t1, space0);

struct StageInput
{
    uint3 groupID : SV_GroupID;
    uint3 groupThreadID : SV_GroupThreadID;
};

SCGridParams scGridDecode(uint4 h0, uint4 h1, uint4 h2)
{
    SCGridParams gp;
    gp.grid_min = asfloat(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = asfloat(h1.xyz);
    gp.cell_size = asfloat(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

SCGridParams wrcGridLoad()
{
    uint4 param = uint4(scgo.Load(0), scgo.Load(4), scgo.Load(8), scgo.Load(12));
    uint4 param_1 = uint4(scgo.Load(16), scgo.Load(20), scgo.Load(24), scgo.Load(28));
    uint4 param_2 = uint4(scgo.Load(32), scgo.Load(36), scgo.Load(40), scgo.Load(44));
    return scGridDecode(param, param_1, param_2);
}

uint scGridCellIndex(SCGridParams gp, float3 p) {
    float3 g = (p - gp.grid_min) / gp.cell_size;
    if (g.x < 0.0 || g.y < 0.0 || g.z < 0.0) return 0xFFFFFFFFu;
    uint3 c = (uint3)(g);
    if (c.x >= gp.dims.x || c.y >= gp.dims.y || c.z >= gp.dims.z) return 0xFFFFFFFFu;
    return c.x + gp.dims.x * (c.y + gp.dims.y * c.z);
}

bool worldToCardUVMarginPad(float3 world_pos, SurfaceCacheCard card, inout float2 out_uv, float margin_uv, float pad_m)
{
    float3 local = world_pos - card.center.xyz;
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
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
        float _330;
        if (axis == 0u)
        {
            _330 = card.extent.y;
        }
        else
        {
            float _338;
            if (axis == 1u)
            {
                _338 = card.extent.x;
            }
            else
            {
                _338 = card.extent.x;
            }
            _330 = _338;
        }
        float eu = _330;
        float _351;
        if (axis == 0u)
        {
            _351 = card.extent.z;
        }
        else
        {
            float _359;
            if (axis == 1u)
            {
                _359 = card.extent.z;
            }
            else
            {
                _359 = card.extent.y;
            }
            _351 = _359;
        }
        float ev = _351;
        float _372;
        if (eu > 0.0001f)
        {
            _372 = pad_m / (eu * 2.0f);
        }
        else
        {
            _372 = 0.0f;
        }
        padU = _372;
        float _383;
        if (ev > 0.0001f)
        {
            _383 = pad_m / (ev * 2.0f);
        }
        else
        {
            _383 = 0.0f;
        }
        padV = _383;
    }
    if (((((u < ((-margin_uv) - padU)) || (u >= ((1.0f + margin_uv) + padU))) || (v < ((-margin_uv) - padV))) || (v >= ((1.0f + margin_uv) + padV))))
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999999f);
    v = clamp(v, 0.0f, 0.999999f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _497;
    if (axis == 0u)
    {
        _497 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _505;
        if (axis == 1u)
        {
            _505 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _505 = float3(0.0f, 0.0f, s);
        }
        _497 = _505;
    }
    return _497;
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
    bool _533 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    float2 atlasUV = param_2;
    if (!_533)
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
    bool _605 = (isnan(depth)) || (isinf(depth));
    if (_605 || (depth < 0.0f))
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
    if (((any(isnan(c))) || (any(isinf(c)))))
    {
        outReject = 6u;
        return false;
    }
    outRadiance = c;
    outKey = (perp + (0.5f * abs(along))) + (hintPenalty * ((perp + abs(along)) + max(tolFloor, 0.05f)));
    return true;
}

uint wrcRejectBin(SCGridParams gp, float3 p) {
    float3 g = (p - gp.grid_min) / max(gp.grid_max - gp.grid_min, (float3)(1e-3));
    uint3 c = (uint3)(clamp(g * 8.0, (float3)(0.0), (float3)(7.0)));
    return c.x + 8u * (c.y + 8u * c.z);
}

bool worldToCardUVMargin(float3 world_pos, SurfaceCacheCard card, out float2 out_uv,
                         float margin_uv) {
    return worldToCardUVMarginPad(world_pos, card, out_uv, margin_uv, 0.0);
}

bool worldToCardUV(float3 world_pos, SurfaceCacheCard card, out float2 out_uv) {
    return worldToCardUVMargin(world_pos, card, out_uv, 0.0);
}

float3 wrcSampleAtlasBilinear(float2 uvPx)
{
    float atlasSize = float(volume_SCParams.y);
    float2 coord = clamp(uvPx, 0.0f.xx, (atlasSize - 1.0f).xx) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 f = clamp(frac(coord), 0.0f.xx, 1.0f.xx);
    int maxC = int(atlasSize) - 1;
    float3 c00 = lighting_atlas.Load(int3(clamp(base, int2(0, 0), maxC.xx), 0)).xyz;
    float3 c10 = lighting_atlas.Load(int3(clamp(base + int2(1, 0), int2(0, 0), maxC.xx), 0)).xyz;
    float3 c01 = lighting_atlas.Load(int3(clamp(base + int2(0, 1), int2(0, 0), maxC.xx), 0)).xyz;
    float3 c11 = lighting_atlas.Load(int3(clamp(base + int2(1, 1), int2(0, 0), maxC.xx), 0)).xyz;
    return ((((c00 * (1.0f - f.x)) * (1.0f - f.y)) + ((c10 * f.x) * (1.0f - f.y))) + ((c01 * (1.0f - f.x)) * f.y)) + ((c11 * f.x) * f.y);
}

float3 EvaluateSkyBase(float3 dir) {
    float up = clamp(dir.y, 0.0, 1.0);
    float3 horizonCol = float3(4.40, 4.20, 3.80); // 暖白地平雾
    float3 zenithCol  = float3(5.50, 5.76, 6.00); // 亮淡蓝天顶
    return lerp(horizonCol, zenithCol, pow(up, 0.45));
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint tid = (input.groupThreadID.y * 8u) + input.groupThreadID.x;
    uint group_id = input.groupID.x;
    if (group_id >= volume_ProbeMeta.w)
    {
        return;
    }
    if (tid >= volume_ProbeMeta.x)
    {
        return;
    }
    uint probeIdx = pul.Load(group_id * 4);
    if (probeIdx >= volume_ProbeMeta.y)
    {
        return;
    }
    uint globalRayIdx = (group_id * volume_ProbeMeta.x) + tid;
    float hitDistance = asfloat(hd.Load(globalRayIdx * 4));
    uint3 counts = volume_ProbeCounts.xyz;
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    float3 probePos = volume_ProbeOrigin.xyz + (float3(float(px), float(py), float(pz)) * volume_ProbeOrigin.w);
    float y = 1.0f - ((2.0f * (float(tid) + 0.5f)) / float(volume_ProbeMeta.x));
    float r = sqrt(max(1.0f - (y * y), 0.0f));
    float ga = 2.3999631f;
    float a = ga * float(tid);
    float3 rayDir = float3(cos(a) * r, y, sin(a) * r);
    float4 result = float4(0.0f, 0.0f, 0.0f, hitDistance);
    if (hitDistance >= 0.0f)
    {
        float3 hitPos = probePos + (rayDir * hitDistance);
        float bestKey = 1000000015047466219876688855040.0f;
        bool found = false;
        if (volume_SCParams.w != 0u)
        {
            uint _1200;
            dbg.InterlockedAdd(0, 1u, _1200);
        }
        if (volume_SCParams.z != 0u)
        {
            if (volume_SCParams.w != 0u)
            {
                uint _1212;
                dbg.InterlockedAdd(4, 1u, _1212);
            }
            uint deviceReads = 0u;
            SCGridParams gp = wrcGridLoad();
            SCGridParams param = gp;
            float3 param_1 = hitPos;
            uint cell = scGridCellIndex(param, param_1);
            if (cell != 4294967295u)
            {
                if (volume_SCParams.w != 0u)
                {
                    uint _1232;
                    dbg.InterlockedAdd(8, 1u, _1232);
                }
                uint begin = scgo.Load((16u + cell) * 4);
                uint end = scgo.Load((17u + cell) * 4);
                if ((((end - begin) > 16u) && (volume_SCParams.w != 0u)))
                {
                    uint _1258;
                    dbg.InterlockedAdd(56, 1u, _1258);
                }
                SurfaceCacheCard card;
                float2 param_4;
                float _1341;
                float3 param_13;
                float param_14;
                uint param_15;
                for (uint gi = begin; (gi < end) && (deviceReads < 16u); gi++)
                {
                    uint cardIdx = scgc.Load(gi * 4);
                    if (cardIdx >= 4096u)
                    {
                        continue;
                    }
                    SurfaceCacheCard _1295;
                    _1295.center = asfloat(cd.Load4(cardIdx * 64));
                    _1295.extent = asfloat(cd.Load4(cardIdx * 64 + 16));
                    _1295.axis_direction = cd.Load(cardIdx * 64 + 32);
                    _1295.resolution = cd.Load(cardIdx * 64 + 36);
                    _1295.atlas_offset_x = cd.Load(cardIdx * 64 + 40);
                    _1295.atlas_offset_y = cd.Load(cardIdx * 64 + 44);
                    _1295.mesh_instance_id = cd.Load(cardIdx * 64 + 48);
                    _1295.static_mesh_slot = cd.Load(cardIdx * 64 + 52);
                    _1295.flags = cd.Load(cardIdx * 64 + 56);
                    _1295.page_card = cd.Load(cardIdx * 64 + 60);
                    card.center = _1295.center;
                    card.extent = _1295.extent;
                    card.axis_direction = _1295.axis_direction;
                    card.resolution = _1295.resolution;
                    card.atlas_offset_x = _1295.atlas_offset_x;
                    card.atlas_offset_y = _1295.atlas_offset_y;
                    card.mesh_instance_id = _1295.mesh_instance_id;
                    card.static_mesh_slot = _1295.static_mesh_slot;
                    card.flags = _1295.flags;
                    card.page_card = _1295.page_card;
                    deviceReads++;
                    if (volume_SCParams.w != 0u)
                    {
                        uint _1324;
                        dbg.InterlockedAdd(16, 1u, _1324);
                    }
                    float3 param_2 = hitPos;
                    SurfaceCacheCard param_3 = card;
                    float param_5 = 0.0f;
                    float param_6 = 0.0f;
                    bool _1334 = worldToCardUVMarginPad(param_2, param_3, param_4, param_5, param_6);
                    float2 strictUV = param_4;
                    bool strictIn = _1334;
                    if (volume_SCQueryParams.x > 0.0f)
                    {
                        _1341 = volume_SCQueryParams.x;
                    }
                    else
                    {
                        _1341 = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.9f);
                    }
                    float kQueryTol = _1341;
                    SurfaceCacheCard param_7 = card;
                    float3 param_8 = hitPos;
                    float3 param_9 = -rayDir;
                    float param_10 = kQueryTol;
                    float param_11 = volume_SCQueryParams.y;
                    float3 param_12 = 0.0f.xxx;
                    bool _1374 = scQueryCardCandidateDbg(param_7, param_8, param_9, param_10, param_11, depth_atlas, lighting_atlas, param_12, param_13, param_14, param_15);
                    float3 scRadiance = param_13;
                    float key = param_14;
                    uint reject = param_15;
                    if (_1374)
                    {
                        if (volume_SCParams.w != 0u)
                        {
                            uint _1386;
                            dbg.InterlockedAdd(60, 1u, _1386);
                            if (!strictIn)
                            {
                                uint _1392;
                                dbg.InterlockedAdd(36, 1u, _1392);
                            }
                        }
                        if (key < bestKey)
                        {
                            bestKey = key;
                            result.x = scRadiance.x;
                            result.y = scRadiance.y;
                            result.z = scRadiance.z;
                            found = true;
                            if (volume_SCParams.w != 0u)
                            {
                                uint _1412;
                                dbg.InterlockedAdd(12, 1u, _1412);
                            }
                        }
                    }
                    else
                    {
                        if (volume_SCParams.w != 0u)
                        {
                            if (reject == 1u)
                            {
                                uint _1424;
                                dbg.InterlockedAdd(20, 1u, _1424);
                                if (!strictIn)
                                {
                                    uint _1430;
                                    dbg.InterlockedAdd(32, 1u, _1430);
                                }
                                if (cardIdx < 4096u)
                                {
                                    uint _1441;
                                    wrcCardFailBuf.InterlockedAdd(cardIdx * 4 + 0, 1u, _1441);
                                }
                                SCGridParams param_16 = gp;
                                float3 param_17 = hitPos;
                                uint rb = wrcRejectBin(param_16, param_17);
                                uint _1455;
                                bins.InterlockedAdd(rb * 16 + 0, 1u, _1455);
                                uint _1458;
                                bins.InterlockedAdd(rb * 16 + 4, 1u, _1458);
                            }
                            else
                            {
                                if ((reject == 2u) || (reject == 7u))
                                {
                                    uint _1468;
                                    dbg.InterlockedAdd(24, 1u, _1468);
                                }
                                else
                                {
                                    if (reject == 3u)
                                    {
                                        uint _1475;
                                        dbg.InterlockedAdd(48, 1u, _1475);
                                    }
                                    else
                                    {
                                        if (reject == 4u)
                                        {
                                            uint _1482;
                                            dbg.InterlockedAdd(40, 1u, _1482);
                                            SCGridParams param_18 = gp;
                                            float3 param_19 = hitPos;
                                            uint _1489;
                                            bins.InterlockedAdd(wrcRejectBin(param_18, param_19) * 16 + 8, 1u, _1489);
                                        }
                                        else
                                        {
                                            if (reject == 5u)
                                            {
                                                uint _1496;
                                                dbg.InterlockedAdd(44, 1u, _1496);
                                                SCGridParams param_20 = gp;
                                                float3 param_21 = hitPos;
                                                uint _1503;
                                                bins.InterlockedAdd(wrcRejectBin(param_20, param_21) * 16 + 8, 1u, _1503);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        bool _1512;
        if (found)
        {
            _1512 = volume_SCParams.w != 0u;
        }
        else
        {
            _1512 = found;
        }
        if (_1512)
        {
            uint _1516;
            dbg.InterlockedAdd(28, 1u, _1516);
        }
        if (!found)
        {
            result.x = 0.0f;
            result.y = 0.0f;
            result.z = 0.0f;
            float fbK = volume_SCQueryParams.z;
            if (((fbK > 0.0f) && (volume_SCParams.z != 0u)))
            {
                uint deviceReads_1 = 0u;
                SCGridParams gpF = wrcGridLoad();
                SCGridParams param_22 = gpF;
                float3 param_23 = probePos;
                uint cellF = scGridCellIndex(param_22, param_23);
                if (cellF != 4294967295u)
                {
                    uint beginF = scgo.Load((16u + cellF) * 4);
                    uint endF = scgo.Load((17u + cellF) * 4);
                    SurfaceCacheCard cardF;
                    float2 param_26;
                    for (uint gi_1 = beginF; (gi_1 < endF) && (deviceReads_1 < 8u); gi_1++)
                    {
                        uint cardIdx_1 = scgc.Load(gi_1 * 4);
                        if (cardIdx_1 >= 4096u)
                        {
                            continue;
                        }
                        SurfaceCacheCard _1588;
                        _1588.center = asfloat(cd.Load4(cardIdx_1 * 64));
                        _1588.extent = asfloat(cd.Load4(cardIdx_1 * 64 + 16));
                        _1588.axis_direction = cd.Load(cardIdx_1 * 64 + 32);
                        _1588.resolution = cd.Load(cardIdx_1 * 64 + 36);
                        _1588.atlas_offset_x = cd.Load(cardIdx_1 * 64 + 40);
                        _1588.atlas_offset_y = cd.Load(cardIdx_1 * 64 + 44);
                        _1588.mesh_instance_id = cd.Load(cardIdx_1 * 64 + 48);
                        _1588.static_mesh_slot = cd.Load(cardIdx_1 * 64 + 52);
                        _1588.flags = cd.Load(cardIdx_1 * 64 + 56);
                        _1588.page_card = cd.Load(cardIdx_1 * 64 + 60);
                        cardF.center = _1588.center;
                        cardF.extent = _1588.extent;
                        cardF.axis_direction = _1588.axis_direction;
                        cardF.resolution = _1588.resolution;
                        cardF.atlas_offset_x = _1588.atlas_offset_x;
                        cardF.atlas_offset_y = _1588.atlas_offset_y;
                        cardF.mesh_instance_id = _1588.mesh_instance_id;
                        cardF.static_mesh_slot = _1588.static_mesh_slot;
                        cardF.flags = _1588.flags;
                        cardF.page_card = _1588.page_card;
                        deviceReads_1++;
                        float3 param_24 = probePos;
                        SurfaceCacheCard param_25 = cardF;
                        bool _1617 = worldToCardUV(param_24, param_25, param_26);
                        float2 atlasUVF = param_26;
                        if (_1617)
                        {
                            float3 fbRad = wrcSampleAtlasBilinear(atlasUVF);
                            if (dot(fbRad, fbRad) > 0.0001f)
                            {
                                float3 capF = ((EvaluateSkyBase(rayDir) * volume_SkyColor.xyz) * volume_SkyColor.w) * fbK;
                                float3 _1646 = min(fbRad, capF);
                                result.x = _1646.x;
                                result.y = _1646.y;
                                result.z = _1646.z;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        if (hitDistance <= (-1.5f))
        {
            float3 _1671 = (EvaluateSkyBase(rayDir) * volume_SkyColor.xyz) * volume_SkyColor.w;
            result.x = _1671.x;
            result.y = _1671.y;
            result.z = _1671.z;
        }
        else
        {
            bool found_1 = false;
            if (volume_SCParams.z != 0u)
            {
                uint deviceReads_2 = 0u;
                SCGridParams gp_1 = wrcGridLoad();
                SCGridParams param_30 = gp_1;
                float3 param_31 = probePos;
                uint cell_1 = scGridCellIndex(param_30, param_31);
                if (cell_1 != 4294967295u)
                {
                    uint begin_1 = scgo.Load((16u + cell_1) * 4);
                    uint end_1 = scgo.Load((17u + cell_1) * 4);
                    SurfaceCacheCard card_1;
                    float2 param_34;
                    for (uint gi_2 = begin_1; (gi_2 < end_1) && (deviceReads_2 < 8u); gi_2++)
                    {
                        uint cardIdx_2 = scgc.Load(gi_2 * 4);
                        if (cardIdx_2 >= 4096u)
                        {
                            continue;
                        }
                        SurfaceCacheCard _1733;
                        _1733.center = asfloat(cd.Load4(cardIdx_2 * 64));
                        _1733.extent = asfloat(cd.Load4(cardIdx_2 * 64 + 16));
                        _1733.axis_direction = cd.Load(cardIdx_2 * 64 + 32);
                        _1733.resolution = cd.Load(cardIdx_2 * 64 + 36);
                        _1733.atlas_offset_x = cd.Load(cardIdx_2 * 64 + 40);
                        _1733.atlas_offset_y = cd.Load(cardIdx_2 * 64 + 44);
                        _1733.mesh_instance_id = cd.Load(cardIdx_2 * 64 + 48);
                        _1733.static_mesh_slot = cd.Load(cardIdx_2 * 64 + 52);
                        _1733.flags = cd.Load(cardIdx_2 * 64 + 56);
                        _1733.page_card = cd.Load(cardIdx_2 * 64 + 60);
                        card_1.center = _1733.center;
                        card_1.extent = _1733.extent;
                        card_1.axis_direction = _1733.axis_direction;
                        card_1.resolution = _1733.resolution;
                        card_1.atlas_offset_x = _1733.atlas_offset_x;
                        card_1.atlas_offset_y = _1733.atlas_offset_y;
                        card_1.mesh_instance_id = _1733.mesh_instance_id;
                        card_1.static_mesh_slot = _1733.static_mesh_slot;
                        card_1.flags = _1733.flags;
                        card_1.page_card = _1733.page_card;
                        deviceReads_2++;
                        float3 param_32 = probePos;
                        SurfaceCacheCard param_33 = card_1;
                        bool _1762 = worldToCardUV(param_32, param_33, param_34);
                        float2 atlasUV = param_34;
                        if (_1762)
                        {
                            float3 missRadiance = wrcSampleAtlasBilinear(atlasUV);
                            if (dot(missRadiance, missRadiance) > 0.0001f)
                            {
                                result.x = missRadiance.x;
                                result.y = missRadiance.y;
                                result.z = missRadiance.z;
                                found_1 = true;
                                if (volume_SCParams.w != 0u)
                                {
                                    uint _1789;
                                    dbg.InterlockedAdd(12, 1u, _1789);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            if (!found_1)
            {
                result.x = 0.0f;
                result.y = 0.0f;
                result.z = 0.0f;
            }
        }
    }
    if (((any(isnan(result.xyz))) || (any(isinf(result.xyz)))))
    {
        result.x = 0.0f;
        result.y = 0.0f;
        result.z = 0.0f;
    }
    float4 _1823 = result;
    float3 _1827 = min(_1823.xyz, 64.0f.xxx);
    result.x = _1827.x;
    result.y = _1827.y;
    result.z = _1827.z;
    if (((any(isnan(result.xyz))) || (any(isinf(result.xyz)))))
    {
        result.x = 0.0f;
        result.y = 0.0f;
        result.z = 0.0f;
    }
    float4 _1854 = result;
    float3 _1856 = min(_1854.xyz, 64.0f.xxx);
    result.x = _1856.x;
    result.y = _1856.y;
    result.z = _1856.z;
    rd.Store4(((probeIdx * volume_ProbeMeta.x) + tid) * 16 + 0, asuint(result));
}
