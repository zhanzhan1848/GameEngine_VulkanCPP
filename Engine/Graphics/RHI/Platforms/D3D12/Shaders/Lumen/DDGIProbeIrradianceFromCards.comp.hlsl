// Vulkan reference: Lumen/DDGIProbeIrradianceFromCards.comp (ddgi_probe_irradiance_from_cards).
struct ProbeContribRange
{
    uint start;
    uint count;
};

struct ProbeCardContrib
{
    uint card_index;
    float w[9];
};

cbuffer DDGIVolumeData : register(b0, space0)
{
    float4 volume_ProbeOrigin : packoffset(c0);
    float volume_ProbeSpacing : packoffset(c1);
    float volume_pad_before_counts_0 : packoffset(c1.y);
    float volume_pad_before_counts_1 : packoffset(c1.z);
    float volume_pad_before_counts_2 : packoffset(c1.w);
    uint4 volume_ProbeCounts : packoffset(c2);
    uint volume_RaysPerProbe : packoffset(c3);
    uint volume_ProbeCountTotal : packoffset(c3.y);
    float volume_IrradianceBlurSigma : packoffset(c3.z);
    float volume_DepthBlurSigma : packoffset(c3.w);
    float volume_DeltaTime : packoffset(c4);
    uint volume_FrameIndex : packoffset(c4.y);
    float volume_RayMaxDistance : packoffset(c4.z);
    float volume_ProbeHysteresis : packoffset(c4.w);
    float volume_TemporalAlpha : packoffset(c5);
    uint volume_ProbeUpdateCount : packoffset(c5.y);
    uint volume_SCLookupCount : packoffset(c5.z);
    uint volume_SCAtlasSize : packoffset(c5.w);
    int volume_ProbeRelocationShiftX : packoffset(c6);
    int volume_ProbeRelocationShiftY : packoffset(c6.y);
    int volume_ProbeRelocationShiftZ : packoffset(c6.z);
    float volume_pad_to_sdf : packoffset(c6.w);
    float4 volume_SdfOrigins[3] : packoffset(c7);
    float4 volume_SdfVoxelSizes[3] : packoffset(c10);
    float4 volume_SdfExtents[3] : packoffset(c13);
    uint4 volume_SdfResolutionsAndCount : packoffset(c16);
    float4 volume_LightDirection : packoffset(c17);
    float4 volume_LightColor : packoffset(c18);
    float4 volume_SkyColor : packoffset(c19);
    float4 volume_Albedo : packoffset(c20);
};

ByteAddressBuffer rangeBuffer : register(t1, space0);
ByteAddressBuffer contribBuffer : register(t2, space0);
ByteAddressBuffer cardRadianceBuffer : register(t3, space0);
ByteAddressBuffer irradianceHistoryBuffer : register(t4, space0);
RWByteAddressBuffer irradianceOutputBuffer : register(u5, space0);
ByteAddressBuffer confidenceBuffer : register(t6, space0);
cbuffer ScInjectParams : register(b7, space0)
{
    float4 sc_inject : packoffset(c0);
};

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    uint probeIdx = input.dispatchThreadID.x;
    if (probeIdx >= volume_ProbeCountTotal)
    {
        return;
    }
    ProbeContribRange _49;
    _49.start = rangeBuffer.Load(probeIdx * 8);
    _49.count = rangeBuffer.Load(probeIdx * 8 + 4);
    ProbeContribRange range;
    range.start = _49.start;
    range.count = _49.count;
    float3 sh[9];
    for (uint i = 0u; i < 9u; i++)
    {
        sh[i] = 0.0f.xxx;
    }
    uint n = min(range.count, 12u);
    float validSum = 0.0f;
    uint validCount = 0u;
    ProbeCardContrib c;
    for (uint i_1 = 0u; i_1 < n; i_1++)
    {
        ProbeCardContrib _108;
        _108.card_index = contribBuffer.Load((range.start + i_1) * 40);
        [unroll]
        for (int _0ident = 0; _0ident < 9; _0ident++)
        {
            _108.w[_0ident] = asfloat(contribBuffer.Load(_0ident * 4 + (range.start + i_1) * 40 + 4));
        }
        c.card_index = _108.card_index;
        c.w[0] = _108.w[0];
        c.w[1] = _108.w[1];
        c.w[2] = _108.w[2];
        c.w[3] = _108.w[3];
        c.w[4] = _108.w[4];
        c.w[5] = _108.w[5];
        c.w[6] = _108.w[6];
        c.w[7] = _108.w[7];
        c.w[8] = _108.w[8];
        float4 cr = asfloat(cardRadianceBuffer.Load4(c.card_index * 16));
        if (cr.w <= 0.0f)
        {
            continue;
        }
        validCount++;
        float3 radiance = cr.xyz;
        validSum += cr.w;
        for (uint b = 0u; b < 9u; b++)
        {
            sh[b] += (radiance * c.w[b]);
        }
    }
    uint base = probeIdx * 9u;
    float alpha = volume_ProbeHysteresis;
    float l0mag = length(sh[0]);
    if (l0mag > 64.0f)
    {
        float s = 64.0f / l0mag;
        for (uint i_2 = 0u; i_2 < 9u; i_2++)
        {
            sh[i_2] *= s;
        }
    }
    float3 sh0 = sh[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.1f);
    float _287;
    float3 scResult[9];
    for (uint i_3 = 0u; i_3 < 9u; i_3++)
    {
        float3 history = asfloat(irradianceHistoryBuffer.Load4((base + i_3) * 16)).xyz;
        uint3 bits = asuint(history);
        if ((((bits.x | bits.y) | bits.z) & 2139095040u) == 2139095040u)
        {
            history = 0.0f.xxx;
        }
        float3 filtered = lerp(history, sh[i_3], alpha.xxx);
        if (i_3 == 0u)
        {
            _287 = 64.0f;
        }
        else
        {
            _287 = (i_3 < 4u) ? l1Limit : l2Limit;
        }
        float limit = _287;
        float mag = length(filtered);
        if (mag > limit)
        {
            filtered *= (limit / mag);
        }
        scResult[i_3] = filtered;
    }
    float3 pathA[9];
    for (uint i_4 = 0u; i_4 < 9u; i_4++)
    {
        pathA[i_4] = asfloat(irradianceOutputBuffer.Load4((base + i_4) * 16)).xyz;
    }
    float countCoverage = clamp(float(validCount) / 6.0f, 0.0f, 1.0f);
    float _350;
    if (validCount > 0u)
    {
        _350 = clamp(validSum / float(validCount), 0.0f, 1.0f);
    }
    else
    {
        _350 = 0.0f;
    }
    float validCoverage = _350;
    float scLum = dot(scResult[0], float3(0.2126f, 0.7152f, 0.0722f));
    float rawPathLum = dot(pathA[0], float3(0.2126f, 0.7152f, 0.0722f));
    float pathLum = max(rawPathLum, 0.0001f);
    float agree = smoothstep(0.0f, 1.0f, scLum / pathLum);
    float convergence = asfloat(confidenceBuffer.Load(input.dispatchThreadID.x * 16 + 12));
    float settled = smoothstep(0.5f, 1.0f, convergence);
    float injectBase = countCoverage * validCoverage;
    float scWeight;
    if (sc_inject.x < 0.5f)
    {
        scWeight = (injectBase * 0.45f) * agree;
    }
    else
    {
        if (sc_inject.x < 1.5f)
        {
            scWeight = (injectBase * agree) * lerp(0.45f, 0.15f, settled);
        }
        else
        {
            float settledWeight = (rawPathLum <= 0.001f) ? 0.15f : 0.0f;
            scWeight = (injectBase * agree) * lerp(0.45f, settledWeight, settled);
        }
    }
    for (uint i_5 = 0u; i_5 < 9u; i_5++)
    {
        float3 blended = lerp(pathA[i_5], scResult[i_5], scWeight.xxx);
        float4 prev = asfloat(irradianceOutputBuffer.Load4((base + i_5) * 16));
        irradianceOutputBuffer.Store4((base + i_5) * 16 + 0, asuint(float4(blended, prev.w)));
    }
}
