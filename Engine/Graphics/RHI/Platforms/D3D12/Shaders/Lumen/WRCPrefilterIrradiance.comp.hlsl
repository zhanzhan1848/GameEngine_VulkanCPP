// Vulkan reference: Lumen/WRCPrefilterIrradiance.comp (wrc_prefilter_irradiance).
#include "../Common/HalfPacking.hlsli"

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

ByteAddressBuffer pd : register(t4, space0);
RWByteAddressBuffer sh : register(u10, space0);

struct StageInput
{
    uint3 groupID : SV_GroupID;
    uint3 groupThreadID : SV_GroupThreadID;
};

float3 wrcUnpackRGB(uint2 p)
{
    float2 lo = unpackHalf2(p.x);
    float2 hi = unpackHalf2(p.y);
    return float3(lo.x, lo.y, hi.x);
}

float3 wrcDir(uint k, uint count) {
    float y = 1.0 - 2.0 * (float(k) + 0.5) / float(count);
    float r = sqrt(max(1.0 - y * y, 0.0));
    float ga = 2.399963229728653;
    float a = ga * float(k);
    return float3(cos(a) * r, y, sin(a) * r);
}

void wrcSHBasis(float3 d, inout float Y[9])
{
    Y[0] = 0.282095f;
    Y[1] = (-0.488603f) * d.y;
    Y[2] = 0.488603f * d.z;
    Y[3] = (-0.488603f) * d.x;
    Y[4] = (1.092548f * d.x) * d.y;
    Y[5] = ((-1.092548f) * d.y) * d.z;
    Y[6] = 0.315392f * (((3.0f * d.z) * d.z) - 1.0f);
    Y[7] = ((-1.092548f) * d.x) * d.z;
    Y[8] = 0.546274f * ((d.x * d.x) - (d.y * d.y));
}

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    uint tid = input.groupThreadID.x;
    uint group_id = input.groupID.x;
    uint probeIdx = (group_id * 64u) + tid;
    if (probeIdx >= volume_ProbeMeta.y)
    {
        return;
    }
    uint K = volume_ProbeMeta.x;
    uint rbase = probeIdx * (K * 2u);
    uint metaBase = (volume_ProbeMeta.y * (K * 2u)) + (probeIdx * 4u);
    uint state = pd.Load((metaBase + 2u) * 4);
    if ((state & 1u) == 0u)
    {
        for (uint i = 0u; i < 9u; i++)
        {
            sh.Store4(((probeIdx * 9u) + i) * 16 + 0, asuint(0.0f.xxxx));
        }
        return;
    }
    float4 acc[9];
    for (uint i_1 = 0u; i_1 < 9u; i_1++)
    {
        acc[i_1] = 0.0f.xxxx;
    }
    float param_4[9];
    for (uint d = 0u; d < K; d++)
    {
        uint2 p0 = uint2(pd.Load(((rbase + (d * 2u)) + 0u) * 4), pd.Load(((rbase + (d * 2u)) + 1u) * 4));
        float3 L = wrcUnpackRGB(p0);
        uint param_1 = d;
        uint param_2 = K;
        float3 param_3 = wrcDir(param_1, param_2);
        wrcSHBasis(param_3, param_4);
        float Y[9] = param_4;
        for (uint i_2 = 0u; i_2 < 9u; i_2++)
        {
            float4 _325 = acc[i_2];
            float3 _327 = _325.xyz + (L * Y[i_2]);
            acc[i_2].x = _327.x;
            acc[i_2].y = _327.y;
            acc[i_2].z = _327.z;
        }
    }
    float mcWeight = 12.566371f / float(K);
    for (uint i_3 = 0u; i_3 < 9u; i_3++)
    {
        sh.Store4(((probeIdx * 9u) + i_3) * 16 + 0, asuint(float4(acc[i_3].xyz * mcWeight, 1.0f)));
    }
}
