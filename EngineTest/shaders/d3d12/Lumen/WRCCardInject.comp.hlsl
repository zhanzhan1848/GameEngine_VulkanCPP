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

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

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
};

RWByteAddressBuffer pd : register(u3, space0);
ByteAddressBuffer cardList : register(t6, space0);
ByteAddressBuffer cards : register(t4, space0);
Texture3D<float4> sdf_cascade_0 : register(t7, space0);
Texture3D<float4> sdf_cascade_1 : register(t8, space0);
Texture3D<float4> sdf_cascade_2 : register(t9, space0);
Texture2D<float4> lighting_atlas : register(t5, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint spvPackHalf2x16(float2 value)
{
    uint2 Packed = f32tof16(value);
    return Packed.x | (Packed.y << 16);
}

float2 spvUnpackHalf2x16(uint value)
{
    return f16tof32(uint2(value & 0xffff, value >> 16));
}

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _132 = local.x >= 0.0f;
    bool _140;
    if (_132)
    {
        _140 = local.x < extent.x;
    }
    else
    {
        _140 = _132;
    }
    bool _146;
    if (_140)
    {
        _146 = local.y >= 0.0f;
    }
    else
    {
        _146 = _140;
    }
    bool _154;
    if (_146)
    {
        _154 = local.y < extent.y;
    }
    else
    {
        _154 = _146;
    }
    bool _160;
    if (_154)
    {
        _160 = local.z >= 0.0f;
    }
    else
    {
        _160 = _154;
    }
    bool _168;
    if (_160)
    {
        _168 = local.z < extent.z;
    }
    else
    {
        _168 = _160;
    }
    return _168;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _179 = uvw.x < 0.0f;
    bool _186;
    if (!_179)
    {
        _186 = uvw.x > 1.0f;
    }
    else
    {
        _186 = _179;
    }
    bool _193;
    if (!_186)
    {
        _193 = uvw.y < 0.0f;
    }
    else
    {
        _193 = _186;
    }
    bool _200;
    if (!_193)
    {
        _200 = uvw.y > 1.0f;
    }
    else
    {
        _200 = _193;
    }
    bool _207;
    if (!_200)
    {
        _207 = uvw.z < 0.0f;
    }
    else
    {
        _207 = _200;
    }
    bool _214;
    if (!_207)
    {
        _214 = uvw.z > 1.0f;
    }
    else
    {
        _214 = _207;
    }
    if (_214)
    {
        return 10000000000.0f;
    }
    float3 p = (uvw * float(resolution)) - 0.5f.xxx;
    int3 base = int3(floor(p));
    float3 f = p - float3(base);
    int3 resI = (int(resolution) - 1).xxx;
    int3 c0 = clamp(base, int3(0, 0, 0), resI);
    int3 c1 = clamp(base + int3(1, 1, 1), int3(0, 0, 0), resI);
    float d000 = sdfTexture.Load(int4(int3(c0.x, c0.y, c0.z), 0)).x;
    float d100 = sdfTexture.Load(int4(int3(c1.x, c0.y, c0.z), 0)).x;
    float d010 = sdfTexture.Load(int4(int3(c0.x, c1.y, c0.z), 0)).x;
    float d110 = sdfTexture.Load(int4(int3(c1.x, c1.y, c0.z), 0)).x;
    float d001 = sdfTexture.Load(int4(int3(c0.x, c0.y, c1.z), 0)).x;
    float d101 = sdfTexture.Load(int4(int3(c1.x, c0.y, c1.z), 0)).x;
    float d011 = sdfTexture.Load(int4(int3(c0.x, c1.y, c1.z), 0)).x;
    float d111 = sdfTexture.Load(int4(int3(c1.x, c1.y, c1.z), 0)).x;
    return lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y), lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
}

float sampleBestSDF(float3 pos)
{
    uint cc = volume_SdfResolutionsAndCount.w;
    bool _391 = cc > 0u;
    bool _408;
    if (_391)
    {
        float3 param = pos;
        float3 param_1 = volume_SdfOrigins[0].xyz;
        float3 param_2 = volume_SdfExtents[0].xyz;
        _408 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _408 = _391;
    }
    if (_408)
    {
        float3 param_3 = pos;
        float3 param_4 = volume_SdfOrigins[0].xyz;
        float3 param_5 = volume_SdfExtents[0].xyz;
        uint param_6 = volume_SdfResolutionsAndCount.x;
        return sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    else
    {
        bool _429 = cc > 1u;
        bool _443;
        if (_429)
        {
            float3 param_7 = pos;
            float3 param_8 = volume_SdfOrigins[1].xyz;
            float3 param_9 = volume_SdfExtents[1].xyz;
            _443 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _443 = _429;
        }
        if (_443)
        {
            float3 param_10 = pos;
            float3 param_11 = volume_SdfOrigins[1].xyz;
            float3 param_12 = volume_SdfExtents[1].xyz;
            uint param_13 = volume_SdfResolutionsAndCount.y;
            return sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13);
        }
        else
        {
            bool _464 = cc > 2u;
            bool _479;
            if (_464)
            {
                float3 param_14 = pos;
                float3 param_15 = volume_SdfOrigins[2].xyz;
                float3 param_16 = volume_SdfExtents[2].xyz;
                _479 = isInsideCascade(param_14, param_15, param_16);
            }
            else
            {
                _479 = _464;
            }
            if (_479)
            {
                float3 param_17 = pos;
                float3 param_18 = volume_SdfOrigins[2].xyz;
                float3 param_19 = volume_SdfExtents[2].xyz;
                uint param_20 = volume_SdfResolutionsAndCount.z;
                return sampleSDFCascade(sdf_cascade_2, param_17, param_18, param_19, param_20);
            }
        }
    }
    return 10000000000.0f;
}

bool wrcVisible(float3 probePos, float3 dir, float cardDist)
{
    float voxel = volume_SdfVoxelSizes[0].x;
    float hitThreshold = max(voxel * 1.25f, 0.449999988079071044921875f);
    float minStep = voxel * 0.25f;
    float stopT = 0.85000002384185791015625f * cardDist;
    bool clearedSkin = false;
    float t = 0.0f;
    for (uint i = 0u; i < 48u; i++)
    {
        float3 param = probePos + (dir * t);
        float d = sampleBestSDF(param);
        if (d >= 1000000000.0f)
        {
            t += max(cardDist * 0.1500000059604644775390625f, 1.0f);
            if (t >= stopT)
            {
                return true;
            }
            continue;
        }
        if (d < hitThreshold)
        {
            if (!clearedSkin)
            {
                t += max(d, minStep);
                if (t >= stopT)
                {
                    return true;
                }
                continue;
            }
            return t >= stopT;
        }
        clearedSkin = true;
        t += clamp(d, minStep, max(voxel * 2.0f, 0.20000000298023223876953125f));
        if (t >= stopT)
        {
            return true;
        }
    }
    return false;
}

float3 wrcDir(uint k, uint K)
{
    float y = 1.0f - ((2.0f * (float(k) + 0.5f)) / float(K));
    float r = sqrt(max(1.0f - (y * y), 0.0f));
    float a = 2.3999631404876708984375f * float(k);
    return float3(cos(a) * r, y, sin(a) * r);
}

float3 wrcUnpackRGB(uint2 p)
{
    float2 lo = spvUnpackHalf2x16(p.x);
    float2 hi = spvUnpackHalf2x16(p.y);
    return float3(lo.x, lo.y, hi.x);
}

uint2 wrcPackRGB(float3 c)
{
    return uint2(spvPackHalf2x16(float2(c.x, c.y)), spvPackHalf2x16(float2(c.z, 0.0f)));
}

void comp_main()
{
    uint pi = gl_GlobalInvocationID.x;
    if (pi >= volume_ProbeMeta.y)
    {
        return;
    }
    uint K = volume_ProbeMeta.x;
    uint rbase = pi * (K * 2u);
    uint metaBase = (volume_ProbeMeta.y * (K * 2u)) + (pi * 4u);
    uint state = pd.Load((metaBase + 2u) * 4 + 0);
    bool _646 = (state & 1u) == 0u;
    bool _653;
    if (!_646)
    {
        _653 = (state & 2u) != 0u;
    }
    else
    {
        _653 = _646;
    }
    if (_653)
    {
        return;
    }
    uint count = cardList.Load((pi * 17u) * 8 + 0);
    if (count == 0u)
    {
        return;
    }
    count = min(count, 16u);
    uint3 counts = volume_ProbeCounts.xyz;
    uint pz = pi / (counts.x * counts.y);
    uint rem = pi % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    float3 probePos = volume_ProbeOrigin.xyz + (float3(float(px), float(py), float(pz)) * volume_ProbeOrigin.w);
    uint atlasSize = volume_SCParams.y;
    float Wacc = 0.0f;
    float3 Lacc = 0.0f.xxx;
    float3 Dacc = 0.0f.xxx;
    SurfaceCacheCard card;
    for (uint i = 0u; i < count; i++)
    {
        uint2 entry = cardList.Load2((((pi * 17u) + 1u) + i) * 8 + 0);
        uint cardIdx = entry.x;
        float w = asfloat(entry.y);
        if (w <= 0.0f)
        {
            break;
        }
        SurfaceCacheCard _771;
        _771.center = asfloat(cards.Load4(cardIdx * 64 + 0));
        _771.extent = asfloat(cards.Load4(cardIdx * 64 + 16));
        _771.axis_direction = cards.Load(cardIdx * 64 + 32);
        _771.resolution = cards.Load(cardIdx * 64 + 36);
        _771.atlas_offset_x = cards.Load(cardIdx * 64 + 40);
        _771.atlas_offset_y = cards.Load(cardIdx * 64 + 44);
        _771.mesh_instance_id = cards.Load(cardIdx * 64 + 48);
        _771.static_mesh_slot = cards.Load(cardIdx * 64 + 52);
        _771.flags = cards.Load(cardIdx * 64 + 56);
        _771.page_card = cards.Load(cardIdx * 64 + 60);
        card.center = _771.center;
        card.extent = _771.extent;
        card.axis_direction = _771.axis_direction;
        card.resolution = _771.resolution;
        card.atlas_offset_x = _771.atlas_offset_x;
        card.atlas_offset_y = _771.atlas_offset_y;
        card.mesh_instance_id = _771.mesh_instance_id;
        card.static_mesh_slot = _771.static_mesh_slot;
        card.flags = _771.flags;
        card.page_card = _771.page_card;
        float3 toCard = card.center.xyz - probePos;
        float d2 = dot(toCard, toCard);
        if (d2 < 9.9999997473787516355514526367188e-05f)
        {
            continue;
        }
        float dist = sqrt(d2);
        float3 dir = toCard / dist.xxx;
        float3 param = probePos;
        float3 param_1 = dir;
        float param_2 = dist;
        if (!wrcVisible(param, param_1, param_2))
        {
            continue;
        }
        uint res = card.resolution;
        float3 acc = 0.0f.xxx;
        for (uint sy = 0u; sy < 4u; sy++)
        {
            for (uint sx = 0u; sx < 4u; sx++)
            {
                uint2 t = uint2(card.atlas_offset_x + ((res * ((sx * 2u) + 1u)) / 8u), card.atlas_offset_y + ((res * ((sy * 2u) + 1u)) / 8u));
                acc += lighting_atlas.Load(int3(int2(min(t, (atlasSize - 1u).xx)), 0)).xyz;
            }
        }
        acc /= 16.0f.xxx;
        float lum = ((0.2125999927520751953125f * acc.x) + (0.715200006961822509765625f * acc.y)) + (0.072200000286102294921875f * acc.z);
        if (lum <= 9.9999997473787516355514526367188e-05f)
        {
            continue;
        }
        Wacc += w;
        Lacc += (acc * w);
        Dacc += (dir * w);
    }
    if (Wacc <= 9.9999997473787516355514526367188e-05f)
    {
        return;
    }
    float dmLen = length(Dacc);
    if (dmLen < 9.9999997473787516355514526367188e-05f)
    {
        return;
    }
    float3 dirAgg = Dacc / dmLen.xxx;
    uint best = 0u;
    float bestDot = -2.0f;
    for (uint k = 0u; k < K; k++)
    {
        uint param_3 = k;
        uint param_4 = K;
        float c = dot(wrcDir(param_3, param_4), dirAgg);
        if (c > bestDot)
        {
            bestDot = c;
            best = k;
        }
    }
    float w_1 = clamp(Wacc, 0.0f, 0.85000002384185791015625f);
    uint slot = rbase + (best * 2u);
    uint2 param_5 = uint2(pd.Load(slot * 4 + 0), pd.Load((slot + 1u) * 4 + 0));
    float3 existing = wrcUnpackRGB(param_5);
    float3 L = min(Lacc / Wacc.xxx, 64.0f.xxx);
    float3 blended = lerp(existing, L, w_1.xxx);
    float3 param_6 = blended;
    uint2 _packed = wrcPackRGB(param_6);
    pd.Store(slot * 4 + 0, _packed.x);
    pd.Store((slot + 1u) * 4 + 0, _packed.y);
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
