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

ByteAddressBuffer pul : register(t5, space0);
RWByteAddressBuffer pd : register(u4, space0);
ByteAddressBuffer hd : register(t6, space0);
ByteAddressBuffer rd : register(t3, space0);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
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
    uint tid = gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    uint slot = (group_id * 64u) + tid;
    if (slot >= volume_ProbeMeta.w)
    {
        return;
    }
    uint probeIdx = pul.Load(slot * 4 + 0);
    if (probeIdx >= volume_ProbeMeta.y)
    {
        return;
    }
    uint K = volume_ProbeMeta.x;
    uint rbase = probeIdx * (K * 2u);
    uint metaBase = (volume_ProbeMeta.y * (K * 2u)) + (probeIdx * 4u);
    if (volume_TraceParams.z < volume_TraceParams.w)
    {
        return;
    }
    uint state = pd.Load((metaBase + 2u) * 4 + 0);
    float skin = max(volume_SdfVoxelSizes[0].x * 1.5f, volume_ProbeOrigin.w * 0.0500000007450580596923828125f);
    float d0 = asfloat(hd.Load(((volume_ProbeMeta.w * K) + slot) * 4 + 0));
    if ((d0 >= 0.0f) && (d0 < skin))
    {
        for (uint d = 0u; d < K; d++)
        {
            pd.Store(((rbase + (d * 2u)) + 0u) * 4 + 0, 0u);
            pd.Store(((rbase + (d * 2u)) + 1u) * 4 + 0, 0u);
        }
        pd.Store((metaBase + 0u) * 4 + 0, 0u);
        pd.Store((metaBase + 1u) * 4 + 0, 0u);
        pd.Store((metaBase + 2u) * 4 + 0, (state & 4294901760u) | 2u);
        return;
    }
    if ((state & 2u) != 0u)
    {
        return;
    }
    float alpha = volume_TraceParams.y;
    bool firstFill = (state & 1u) == 0u;
    uint meanU = pd.Load((metaBase + 0u) * 4 + 0);
    uint varU = pd.Load((metaBase + 1u) * 4 + 0);
    float _244;
    if (firstFill)
    {
        _244 = 0.0f;
    }
    else
    {
        _244 = asfloat(meanU);
    }
    float meanD = _244;
    float _253;
    if (firstFill)
    {
        _253 = 0.0f;
    }
    else
    {
        _253 = asfloat(varU);
    }
    float varD = _253;
    bool haveStats = ((!firstFill) && (meanU != 0u)) && (varU != 0u);
    float sumD = 0.0f;
    float sumD2 = 0.0f;
    float wSum = 0.0f;
    float3 _363;
    for (uint d_1 = 0u; d_1 < K; d_1++)
    {
        float4 raySample = asfloat(rd.Load4(((probeIdx * K) + d_1) * 16 + 0));
        float dist = raySample.w;
        float3 rad = max(raySample.xyz, 0.0f.xxx);
        bool _306 = any(isnan(rad));
        bool _313;
        if (!_306)
        {
            _313 = any(isinf(rad));
        }
        else
        {
            _313 = _306;
        }
        if (_313)
        {
            rad = 0.0f.xxx;
            dist = -1.0f;
        }
        uint2 oldPacked = uint2(pd.Load(((rbase + (d_1 * 2u)) + 0u) * 4 + 0), pd.Load(((rbase + (d_1 * 2u)) + 1u) * 4 + 0));
        uint2 param = oldPacked;
        float3 oldRad = wrcUnpackRGB(param);
        float w = 1.0f;
        if (haveStats && (dist >= 0.0f))
        {
            float dd = max(dist - meanD, 0.0f);
            w = clamp((varD / (varD + (dd * dd))) * 2.0f, 0.20000000298023223876953125f, 1.0f);
        }
        if (firstFill)
        {
            _363 = rad;
        }
        else
        {
            _363 = lerp(oldRad, rad, (alpha * w).xxx);
        }
        float3 blended = _363;
        float3 param_1 = min(blended, 64.0f.xxx);
        uint2 _packed = wrcPackRGB(param_1);
        pd.Store(((rbase + (d_1 * 2u)) + 0u) * 4 + 0, _packed.x);
        pd.Store(((rbase + (d_1 * 2u)) + 1u) * 4 + 0, _packed.y);
        if (dist >= 0.0f)
        {
            sumD += (w * dist);
            sumD2 += ((w * dist) * dist);
            wSum += w;
        }
    }
    if (wSum > 0.0f)
    {
        float newMean = sumD / wSum;
        float newVar = max((sumD2 / wSum) - (newMean * newMean), 0.0f);
        float _439;
        if (firstFill)
        {
            _439 = newMean;
        }
        else
        {
            _439 = lerp(meanD, newMean, alpha);
        }
        float outMean = _439;
        float _451;
        if (firstFill)
        {
            _451 = newVar;
        }
        else
        {
            _451 = lerp(varD, newVar, alpha);
        }
        float outVar = _451;
        pd.Store((metaBase + 0u) * 4 + 0, asuint(outMean));
        pd.Store((metaBase + 1u) * 4 + 0, asuint(outVar));
    }
    pd.Store((metaBase + 2u) * 4 + 0, state | 1u);
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
