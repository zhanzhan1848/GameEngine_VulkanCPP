// Vulkan reference: Lumen/DDGIUpdateIrradiance.comp (ddgi_update_irradiance).
struct DDGIRayData
{
    float4 radiance_and_dist;
};

cbuffer DDGIVolumeData : register(b1, space0)
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
    int volume_RaySampleMode : packoffset(c6.w);
    float4 volume_SdfOrigins[3] : packoffset(c7);
    float4 volume_SdfVoxelSizes[3] : packoffset(c10);
    float4 volume_SdfExtents[3] : packoffset(c13);
    uint4 volume_SdfResolutionsAndCount : packoffset(c16);
    float4 volume_LightDirection : packoffset(c17);
    float4 volume_LightColor : packoffset(c18);
};

ByteAddressBuffer probeUpdateList : register(t5, space0);
ByteAddressBuffer rayBuffer : register(t2, space0);
RWByteAddressBuffer confidenceBuffer : register(u6, space0);
ByteAddressBuffer irradianceHistory : register(t3, space0);
RWByteAddressBuffer irradianceOutput : register(u4, space0);
RWByteAddressBuffer probeOpenSky : register(u7, space0);
cbuffer GlobalShaderData : register(b0, space0)
{
    row_major float4x4 globalData_View : packoffset(c0);
    row_major float4x4 globalData_Projection : packoffset(c4);
    row_major float4x4 globalData_InvProjection : packoffset(c8);
    row_major float4x4 globalData_ViewProjection : packoffset(c12);
    row_major float4x4 globalData_PreviousViewProjection : packoffset(c16);
    row_major float4x4 globalData_InvViewProjection : packoffset(c20);
    float4 globalData_CameraPositionAndViewWidth : packoffset(c24);
    float4 globalData_CameraDirectionAndViewHeight : packoffset(c25);
    uint globalData_NumDirectionalLights : packoffset(c26);
    float globalData_DeltaTime : packoffset(c26.y);
    float globalData_FrameCount : packoffset(c26.z);
    uint globalData_padding : packoffset(c26.w);
};

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

uint ddgiOctSampleHash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

float4 ddgiOctStratifiedSample(uint rayIndex, uint rayCount, uint frameIndex)
{
    uint cell = rayIndex / (rayCount / 64u);
    uint param = frameIndex ^ 2654435769u;
    uint _63 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _68 = ddgiOctSampleHash(param_1);
    uint seed = _63 ^ _68;
    uint param_2 = seed ^ 1757159915u;
    uint _77 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _85 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_77 >> 14u), float(_85 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _140 = q;
        float3 _148 = q;
        bool2 _152 = bool2(_148.xy.x >= 0.0f.xx.x, _148.xy.y >= 0.0f.xx.y);
        float2 _154 = (1.0f.xx - abs(_140.yx)) * float2(_152.x ? 1.0f.xx.x : (-1.0f).xx.x, _152.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _154.x;
        q.y = _154.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        if (((rayCount >= 64u) && ((rayCount % 64u) == 0u)))
        {
            uint param = rayIndex;
            uint param_1 = rayCount;
            uint param_2 = frameIndex;
            return ddgiOctStratifiedSample(param, param_1, param_2);
        }
        mode = 0u;
    }
    float u = frac((float(rayIndex) + 0.5f) * 0.618034f);
    float v = frac(((float(rayIndex) + 0.5f) * 0.618034f) * 0.618034f);
    if ((((mode != 0u) && (mode != 3u)) && (mode != 4u)) && (mode != 5u))
    {
        u = frac(u + (float(frameIndex) * 0.618034f));
        v = frac(v + ((float(frameIndex) * 0.618034f) * 0.618034f));
    }
    if (mode == 3u)
    {
        v = (float(rayIndex) + 0.5f) / max(float(rayCount), 1.0f);
    }
    if (mode == 2u)
    {
        float3 w = normalize(toLight);
        float s = (w.z >= 0.0f) ? 1.0f : (-1.0f);
        float a = (-1.0f) / (s + w.z);
        float b = (w.x * w.y) * a;
        float3 tangent = float3(1.0f + (((s * w.x) * w.x) * a), s * b, (-s) * w.x);
        float3 bitangent = float3(b, s + ((w.y * w.y) * a), -w.y);
        float3 dir;
        if (v < 0.5f)
        {
            float xi = v * 2.0f;
            float cosT = sqrt(max(1.0f - xi, 0.0f));
            float sinT = sqrt(max(1.0f - (cosT * cosT), 0.0f));
            float th = 6.2831855f * u;
            dir = ((w * cosT) + (tangent * (sinT * cos(th)))) + (bitangent * (sinT * sin(th)));
        }
        else
        {
            float vu = (v - 0.5f) * 2.0f;
            float ph = acos(clamp(1.0f - (2.0f * vu), -1.0f, 1.0f));
            float sp = sin(ph);
            float th_1 = 6.2831855f * u;
            dir = float3(sp * cos(th_1), sp * sin(th_1), cos(ph));
        }
        float cosL = max(dot(dir, w), 0.0f);
        float pdf = max(((0.5f * cosL) / 3.1415927f) + 0.039788734f, 1e-05f);
        return float4(dir, pdf);
    }
    float theta = 6.2831855f * u;
    float phi = acos(clamp(1.0f - (2.0f * v), -1.0f, 1.0f));
    float sinPhi = sin(phi);
    return float4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi), 0.07957747f);
}

[numthreads(64, 1, 1)]
void main(StageInput input)
{
    uint gid = input.dispatchThreadID.x;
    if (gid >= volume_ProbeUpdateCount)
    {
        return;
    }
    uint probeIdx = probeUpdateList.Load(gid * 4);
    uint rayOffset = probeIdx * volume_RaysPerProbe;
    float3 shAccum[9];
    for (uint i = 0u; i < 9u; i++)
    {
        shAccum[i] = 0.0f.xxx;
    }
    uint escapeCount = 0u;
    float skyEnergySum = 0.0f;
    float totalEnergySum = 0.0f;
    bool _530;
    float3 _614;
    float basis[9];
    float _722;
    for (uint r = 0u; r < volume_RaysPerProbe; r++)
    {
        uint rayIdx = rayOffset + r;
        float3 radiance = asfloat(rayBuffer.Load4(rayIdx * 16)).xyz;
        float hitDist = asfloat(rayBuffer.Load(rayIdx * 16 + 12));
        if (volume_RaySampleMode == 7)
        {
            _530 = hitDist == (-2.0f);
        }
        else
        {
            _530 = hitDist <= (-1.5f);
        }
        if (_530)
        {
            escapeCount++;
            skyEnergySum += dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            totalEnergySum += dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
        }
        else
        {
            if (hitDist < 0.0f)
            {
            }
            else
            {
                float distWeight = 1.0f - smoothstep(volume_RayMaxDistance * 0.8f, volume_RayMaxDistance, hitDist);
                radiance *= max(distWeight, 0.2f);
                float bail = smoothstep(0.0f, volume_SdfVoxelSizes[0].x * 2.0f, hitDist);
                radiance *= bail;
                totalEnergySum += dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            }
        }
        if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 1e-08f)
        {
            _614 = normalize(-volume_LightDirection.xyz);
        }
        else
        {
            _614 = float3(0.0f, -1.0f, 0.0f);
        }
        float3 toLight = _614;
        uint param = r;
        uint param_1 = volume_RaysPerProbe;
        uint param_2 = volume_FrameIndex;
        uint param_3 = uint(volume_RaySampleMode);
        float3 param_4 = toLight;
        float4 _642 = ddgiSampleRayDirection(param, param_1, param_2, param_3, param_4);
        float4 raySample = _642;
        float3 d = raySample.xyz;
        float x = d.x;
        float y = d.y;
        float z = d.z;
        float x2 = x * x;
        float y2 = y * y;
        float z2 = z * z;
        basis[0] = 0.282095f;
        basis[1] = (-0.488603f) * y;
        basis[2] = 0.488603f * z;
        basis[3] = (-0.488603f) * x;
        basis[4] = (1.092548f * y) * x;
        basis[5] = ((-1.092548f) * y) * z;
        basis[6] = 0.315392f * ((3.0f * z2) - 1.0f);
        basis[7] = ((-1.092548f) * x) * z;
        basis[8] = 0.546274f * (x2 - y2);
        if (volume_RaySampleMode == 0)
        {
            _722 = 12.566371f / float(volume_RaysPerProbe);
        }
        else
        {
            _722 = 1.0f / (float(volume_RaysPerProbe) * max(raySample.w, 1e-05f));
        }
        float mcWeight = _722;
        for (uint i_1 = 0u; i_1 < 9u; i_1++)
        {
            shAccum[i_1] += ((radiance * basis[i_1]) * mcWeight);
        }
    }
    uint probeBase = probeIdx * 9u;
    uint confidenceBase = probeIdx * 4u;
    float conv = asfloat(confidenceBuffer.Load((confidenceBase + 3u) * 4));
    bool _784;
    if (volume_RaySampleMode == 7)
    {
        _784 = conv <= 0.0f;
    }
    else
    {
        _784 = conv < 0.5f;
    }
    bool firstUpdate = _784;
    float settledAlpha = clamp(volume_TemporalAlpha, 0.0f, 1.0f);
    float _800;
    if (firstUpdate)
    {
        _800 = 1.0f;
    }
    else
    {
        _800 = lerp(0.35f, settledAlpha, min(conv * 2.0f, 1.0f));
    }
    float alpha = _800;
    float3 sh0 = shAccum[0];
    float l1Limit = max(min(length(sh0) * 3.0f, 192.0f), 0.1f);
    float l2Limit = max(min(length(sh0) * 2.0f, 128.0f), 0.05f);
    for (uint i_2 = 0u; i_2 < 9u; i_2++)
    {
        float3 history = asfloat(irradianceHistory.Load4((probeBase + i_2) * 16)).xyz;
        float3 filtered = lerp(history, shAccum[i_2], alpha.xxx);
        if (i_2 == 0u)
        {
            float mag = length(filtered);
            if (mag > 64.0f)
            {
                filtered *= (64.0f / mag);
            }
        }
        else
        {
            if (i_2 < 4u)
            {
                float mag_1 = length(filtered);
                if (mag_1 > l1Limit)
                {
                    filtered *= (l1Limit / mag_1);
                }
            }
            else
            {
                float mag_2 = length(filtered);
                if (mag_2 > l2Limit)
                {
                    filtered *= (l2Limit / mag_2);
                }
            }
        }
        irradianceOutput.Store4((probeBase + i_2) * 16 + 0, asuint(float4(filtered, 0.0f)));
    }
    confidenceBuffer.Store((confidenceBase + 3u) * 4 + 0, asuint(min(conv + 0.0333f, 1.0f)));
    float curOpen = float(escapeCount) / float(volume_RaysPerProbe);
    float2 prevOpen = asfloat(probeOpenSky.Load2(probeIdx * 8));
    float _947;
    if (totalEnergySum > 1e-06f)
    {
        _947 = clamp(skyEnergySum / totalEnergySum, 0.0f, 1.0f);
    }
    else
    {
        _947 = prevOpen.y;
    }
    float curSer = _947;
    bool firstPublish = ((prevOpen.x <= 0.0f) && (curOpen > 0.0f)) && (conv < 0.5f);
    float _970;
    if (firstPublish)
    {
        _970 = curOpen;
    }
    else
    {
        _970 = lerp(prevOpen.x, curOpen, 0.35f);
    }
    float newOpen = _970;
    float _983;
    if (totalEnergySum > 1e-06f)
    {
        float _987;
        if (firstPublish)
        {
            _987 = curSer;
        }
        else
        {
            _987 = lerp(prevOpen.y, curSer, 0.35f);
        }
        _983 = _987;
    }
    else
    {
        _983 = prevOpen.y;
    }
    float newSer = _983;
    probeOpenSky.Store2(probeIdx * 8 + 0, asuint(float2(newOpen, newSer)));
}
