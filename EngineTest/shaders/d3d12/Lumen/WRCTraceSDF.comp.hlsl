static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer WRCVolumeDataBlock : register(b3, space0)
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

ByteAddressBuffer pul : register(t5, space0);
RWByteAddressBuffer hd : register(u4, space0);
Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _84 = local.x >= 0.0f;
    bool _92;
    if (_84)
    {
        _92 = local.x < extent.x;
    }
    else
    {
        _92 = _84;
    }
    bool _99;
    if (_92)
    {
        _99 = local.y >= 0.0f;
    }
    else
    {
        _99 = _92;
    }
    bool _107;
    if (_99)
    {
        _107 = local.y < extent.y;
    }
    else
    {
        _107 = _99;
    }
    bool _114;
    if (_107)
    {
        _114 = local.z >= 0.0f;
    }
    else
    {
        _114 = _107;
    }
    bool _122;
    if (_114)
    {
        _122 = local.z < extent.z;
    }
    else
    {
        _122 = _114;
    }
    return _122;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _133 = uvw.x < 0.0f;
    bool _140;
    if (!_133)
    {
        _140 = uvw.x > 1.0f;
    }
    else
    {
        _140 = _133;
    }
    bool _147;
    if (!_140)
    {
        _147 = uvw.y < 0.0f;
    }
    else
    {
        _147 = _140;
    }
    bool _154;
    if (!_147)
    {
        _154 = uvw.y > 1.0f;
    }
    else
    {
        _154 = _147;
    }
    bool _161;
    if (!_154)
    {
        _161 = uvw.z < 0.0f;
    }
    else
    {
        _161 = _154;
    }
    bool _168;
    if (!_161)
    {
        _168 = uvw.z > 1.0f;
    }
    else
    {
        _168 = _161;
    }
    if (_168)
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
    bool _345 = cc > 0u;
    bool _362;
    if (_345)
    {
        float3 param = pos;
        float3 param_1 = volume_SdfOrigins[0].xyz;
        float3 param_2 = volume_SdfExtents[0].xyz;
        _362 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _362 = _345;
    }
    if (_362)
    {
        float3 param_3 = pos;
        float3 param_4 = volume_SdfOrigins[0].xyz;
        float3 param_5 = volume_SdfExtents[0].xyz;
        uint param_6 = volume_SdfResolutionsAndCount.x;
        return sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    else
    {
        bool _383 = cc > 1u;
        bool _397;
        if (_383)
        {
            float3 param_7 = pos;
            float3 param_8 = volume_SdfOrigins[1].xyz;
            float3 param_9 = volume_SdfExtents[1].xyz;
            _397 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _397 = _383;
        }
        if (_397)
        {
            float3 param_10 = pos;
            float3 param_11 = volume_SdfOrigins[1].xyz;
            float3 param_12 = volume_SdfExtents[1].xyz;
            uint param_13 = volume_SdfResolutionsAndCount.y;
            return sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13);
        }
        else
        {
            bool _418 = cc > 2u;
            bool _433;
            if (_418)
            {
                float3 param_14 = pos;
                float3 param_15 = volume_SdfOrigins[2].xyz;
                float3 param_16 = volume_SdfExtents[2].xyz;
                _433 = isInsideCascade(param_14, param_15, param_16);
            }
            else
            {
                _433 = _418;
            }
            if (_433)
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

float3 wrcSampleDirection(uint k, uint count)
{
    float y = 1.0f - ((2.0f * (float(k) + 0.5f)) / float(count));
    float r = sqrt(max(1.0f - (y * y), 0.0f));
    float ga = 2.3999631404876708984375f;
    float a = ga * float(k);
    return float3(cos(a) * r, y, sin(a) * r);
}

void comp_main()
{
    uint tid = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    if (group_id >= volume_ProbeMeta.w)
    {
        return;
    }
    if (tid >= volume_ProbeMeta.x)
    {
        return;
    }
    uint probeIdx = pul.Load(group_id * 4 + 0);
    if (probeIdx >= volume_ProbeMeta.y)
    {
        return;
    }
    uint3 counts = volume_ProbeCounts.xyz;
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    float3 probePos = volume_ProbeOrigin.xyz + (float3(float(px), float(py), float(pz)) * volume_ProbeOrigin.w);
    if (tid == 0u)
    {
        float3 param = probePos;
        float d0 = sampleBestSDF(param);
        hd.Store(((volume_ProbeMeta.w * volume_ProbeMeta.x) + group_id) * 4 + 0, asuint((d0 >= 1000000000.0f) ? 1000000000.0f : d0));
    }
    uint param_1 = tid;
    uint param_2 = volume_ProbeMeta.x;
    float3 rayDir = wrcSampleDirection(param_1, param_2);
    float t = 0.0f;
    float hitDistance = -1.0f;
    float hitThreshold = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
    float minStep = volume_SdfVoxelSizes[0].x * 0.25f;
    uint coarsest = min(2u, (volume_SdfResolutionsAndCount.w - 1u));
    float fallbackStride = volume_SdfExtents[coarsest].x * 0.100000001490116119384765625f;
    float prevD = 3.3999999521443642490773241379936e+38f;
    float prevT = 0.0f;
    bool clearedSkin = false;
    bool signedCoarseField = volume_ProbeCounts.w != 0u;
    float advance;
    for (uint _step = 0u; _step < 48u; _step++)
    {
        float3 pos = probePos + (rayDir * t);
        float3 param_3 = pos;
        float d = sampleBestSDF(param_3);
        if (d >= 1000000000.0f)
        {
            t += fallbackStride;
            if (t > volume_TraceParams.x)
            {
                hitDistance = -2.0f;
                break;
            }
            continue;
        }
        if ((((signedCoarseField && clearedSkin) && (d <= 0.0f)) && (prevD > 0.0f)) && (prevD < 1000000000.0f))
        {
            float tLo = prevT;
            float tHi = t;
            for (int b = 0; b < 5; b++)
            {
                float tMid = 0.5f * (tLo + tHi);
                float3 param_4 = probePos + (rayDir * tMid);
                float dMid = sampleBestSDF(param_4);
                if (dMid <= 0.0f)
                {
                    tHi = tMid;
                }
                else
                {
                    tLo = tMid;
                }
            }
            hitDistance = tHi;
            break;
        }
        if (d < hitThreshold)
        {
            if (!clearedSkin)
            {
                t += max(d, minStep);
                if (t > volume_TraceParams.x)
                {
                    hitDistance = -2.0f;
                    break;
                }
                continue;
            }
            hitDistance = t;
            break;
        }
        clearedSkin = true;
        if ((d > 6.0f) && (d >= prevD))
        {
            advance = d * 1.5f;
        }
        else
        {
            advance = clamp(d, minStep, max(volume_SdfVoxelSizes[0].x * 2.0f, 0.20000000298023223876953125f));
        }
        prevD = d;
        prevT = t;
        t += advance;
        if (t > volume_TraceParams.x)
        {
            hitDistance = -2.0f;
            break;
        }
    }
    hd.Store(((group_id * volume_ProbeMeta.x) + tid) * 4 + 0, asuint(hitDistance));
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
