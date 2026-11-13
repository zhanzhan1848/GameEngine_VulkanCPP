struct DDGIRayData
{
    float4 radiance_and_dist;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

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

ByteAddressBuffer _506 : register(t5, space0);
ByteAddressBuffer _559 : register(t2, space0);
ByteAddressBuffer _833 : register(t6, space0);
RWByteAddressBuffer _1008 : register(u4, space0);
ByteAddressBuffer _1100 : register(t3, space0);
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


static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint ddgiOctSampleHash(inout uint x)
{
    x ^= (x >> 16u);
    x *= 2146121005u;
    x ^= (x >> 15u);
    x *= 2221713035u;
    return x ^ (x >> 16u);
}

float4 ddgiOctStratifiedSample(uint rayIndex, uint rayCount, uint frameIndex)
{
    uint cell = rayIndex / (rayCount / 64u);
    uint param = frameIndex ^ 2654435769u;
    uint _68 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _73 = ddgiOctSampleHash(param_1);
    uint seed = _68 ^ _73;
    uint param_2 = seed ^ 1757159915u;
    uint _81 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _89 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_81 >> 14u), float(_89 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _144 = q;
        float3 _152 = q;
        bool2 _156 = bool2(_152.xy.x >= 0.0f.xx.x, _152.xy.y >= 0.0f.xx.y);
        float2 _158 = (1.0f.xx - abs(_144.yx)) * float2(_156.x ? 1.0f.xx.x : (-1.0f).xx.x, _156.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _158.x;
        q.y = _158.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _189 = rayCount >= 64u;
        bool _195;
        if (_189)
        {
            _195 = (rayCount % 64u) == 0u;
        }
        else
        {
            _195 = _189;
        }
        if (_195)
        {
            uint param = rayIndex;
            uint param_1 = rayCount;
            uint param_2 = frameIndex;
            return ddgiOctStratifiedSample(param, param_1, param_2);
        }
        mode = 0u;
    }
    float u = frac((float(rayIndex) + 0.5f) * 0.61803400516510009765625f);
    float v = frac(((float(rayIndex) + 0.5f) * 0.61803400516510009765625f) * 0.61803400516510009765625f);
    if ((((mode != 0u) && (mode != 3u)) && (mode != 4u)) && (mode != 5u))
    {
        u = frac(u + (float(frameIndex) * 0.61803400516510009765625f));
        v = frac(v + ((float(frameIndex) * 0.61803400516510009765625f) * 0.61803400516510009765625f));
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
            float th = 6.283185482025146484375f * u;
            dir = ((w * cosT) + (tangent * (sinT * cos(th)))) + (bitangent * (sinT * sin(th)));
        }
        else
        {
            float vu = (v - 0.5f) * 2.0f;
            float ph = acos(clamp(1.0f - (2.0f * vu), -1.0f, 1.0f));
            float sp = sin(ph);
            float th_1 = 6.283185482025146484375f * u;
            dir = float3(sp * cos(th_1), sp * sin(th_1), cos(ph));
        }
        float cosL = max(dot(dir, w), 0.0f);
        float pdf = max(((0.5f * cosL) / 3.1415927410125732421875f) + 0.0397887341678142547607421875f, 9.9999997473787516355514526367188e-06f);
        return float4(dir, pdf);
    }
    float theta = 6.283185482025146484375f * u;
    float phi = acos(clamp(1.0f - (2.0f * v), -1.0f, 1.0f));
    float sinPhi = sin(phi);
    return float4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi), 0.079577468335628509521484375f);
}

float2 octahedralEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 uv = d.xy / l1norm.xx;
    if (d.z < 0.0f)
    {
        float2 s = lerp((-1.0f).xx, 1.0f.xx, float2(bool2(uv.x >= 0.0f.xx.x, uv.y >= 0.0f.xx.y)));
        uv = (1.0f.xx - abs(uv.yx)) * s;
    }
    return (uv * 0.5f) + 0.5f.xx;
}

void comp_main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= volume_ProbeUpdateCount)
    {
        return;
    }
    uint probeIdx = _506.Load(gid * 4 + 0);
    uint texels = 64u;
    float sumWeight[64];
    float sumDist[64];
    float sumDistSq[64];
    uint rayCountArr[64];
    for (uint t = 0u; t < texels; t++)
    {
        sumWeight[t] = 0.0f;
        sumDist[t] = 0.0f;
        sumDistSq[t] = 0.0f;
        rayCountArr[t] = 0u;
    }
    uint res = 8u;
    DDGIRayData ray;
    float3 _613;
    for (uint r = 0u; r < volume_RaysPerProbe; r++)
    {
        DDGIRayData _568;
        _568.radiance_and_dist = asfloat(_559.Load4(((probeIdx * volume_RaysPerProbe) + r) * 16 + 0));
        ray.radiance_and_dist = _568.radiance_and_dist;
        bool _577 = volume_RaySampleMode != 7;
        bool _583;
        if (_577)
        {
            _583 = ray.radiance_and_dist.w < 0.0f;
        }
        else
        {
            _583 = _577;
        }
        if (_583)
        {
            continue;
        }
        if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
        {
            _613 = normalize(-volume_LightDirection.xyz);
        }
        else
        {
            _613 = float3(0.0f, -1.0f, 0.0f);
        }
        uint param = r;
        uint param_1 = volume_RaysPerProbe;
        uint param_2 = volume_FrameIndex;
        uint param_3 = uint(volume_RaySampleMode);
        float3 param_4 = _613;
        float4 _635 = ddgiSampleRayDirection(param, param_1, param_2, param_3, param_4);
        float4 raySample = _635;
        float3 param_5 = raySample.xyz;
        float2 uv = octahedralEncode(param_5);
        uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u), clamp(uint(uv.y * float(res)), 0u, res - 1u));
        uint texelIdx = (texel.y * res) + texel.x;
        float dist = ray.radiance_and_dist.w;
        float weight = 1.0f;
        if (volume_RaySampleMode == 7)
        {
            if ((dist == (-2.0f)) || (dist == (-3.0f)))
            {
                dist = volume_RayMaxDistance;
            }
            bool _693 = dist < 0.0f;
            bool _699;
            if (!_693)
            {
                _699 = isnan(dist);
            }
            else
            {
                _699 = _693;
            }
            bool _705;
            if (!_699)
            {
                _705 = isinf(dist);
            }
            else
            {
                _705 = _699;
            }
            if (_705)
            {
                continue;
            }
            weight = 0.25f / raySample.w;
            float oldWeight = sumWeight[texelIdx];
            float newWeight = oldWeight + weight;
            if (oldWeight == 0.0f)
            {
                sumDist[texelIdx] = dist;
                sumDistSq[texelIdx] = 0.0f;
            }
            else
            {
                float delta = dist - sumDist[texelIdx];
                float newMean = sumDist[texelIdx] + (delta * (weight / newWeight));
                sumDistSq[texelIdx] += ((weight * delta) * (dist - newMean));
                sumDist[texelIdx] = newMean;
            }
            sumWeight[texelIdx] = newWeight;
        }
        else
        {
            sumDist[texelIdx] += dist;
            sumDistSq[texelIdx] += (dist * dist);
        }
        rayCountArr[texelIdx]++;
    }
    if (volume_RaySampleMode == 5)
    {
        uint extraBase = (volume_ProbeCountTotal * volume_RaysPerProbe) + (gid * 128u);
        for (uint cell = 0u; cell < texels; cell++)
        {
            if (rayCountArr[cell] != 0u)
            {
                continue;
            }
            for (uint sampleIdx = 0u; sampleIdx < 2u; sampleIdx++)
            {
                float _distance = asfloat(_833.Load(((extraBase + (cell * 2u)) + sampleIdx) * 4 + 0));
                if (_distance == (-2.0f))
                {
                    _distance = volume_RayMaxDistance;
                }
                if (_distance < 0.0f)
                {
                    continue;
                }
                sumDist[cell] += _distance;
                sumDistSq[cell] += (_distance * _distance);
                rayCountArr[cell]++;
            }
        }
    }
    uint sampledArr[64];
    if (volume_RaySampleMode == 4)
    {
        for (uint t_1 = 0u; t_1 < texels; t_1++)
        {
            sampledArr[t_1] = 0u;
        }
        for (uint r_1 = 0u; r_1 < volume_RaysPerProbe; r_1++)
        {
            uint param_6 = r_1;
            uint param_7 = volume_RaysPerProbe;
            uint param_8 = volume_FrameIndex;
            uint param_9 = 4u;
            float3 param_10 = float3(0.0f, -1.0f, 0.0f);
            float4 _917 = ddgiSampleRayDirection(param_6, param_7, param_8, param_9, param_10);
            float3 rayDir = _917.xyz;
            float3 param_11 = rayDir;
            float2 uv_1 = octahedralEncode(param_11);
            uint2 cell_1 = uint2(clamp(uint(uv_1.x * float(res)), 0u, res - 1u), clamp(uint(uv_1.y * float(res)), 0u, res - 1u));
            sampledArr[(cell_1.y * res) + cell_1.x] = 1u;
        }
    }
    uint floatsPerProbe = texels * 2u;
    uint probeBase = probeIdx * floatsPerProbe;
    float t_2 = clamp(float(max(volume_FrameIndex, 1u) - 1u) / 60.0f, 0.0f, 1.0f);
    float alpha = lerp(1.0f, volume_DepthBlurSigma, t_2);
    float _1234;
    float _1247;
    for (uint texelIdx_1 = 0u; texelIdx_1 < texels; texelIdx_1++)
    {
        uint meanIdx = probeBase + texelIdx_1;
        uint varIdx = (probeBase + texels) + texelIdx_1;
        if (rayCountArr[texelIdx_1] == 0u)
        {
            if (volume_RaySampleMode == 7)
            {
                _1008.Store(meanIdx * 4 + 0, asuint(-1.0f));
                _1008.Store(varIdx * 4 + 0, asuint(0.0f));
                continue;
            }
            bool _1016 = volume_RaySampleMode == 4;
            bool _1023;
            if (_1016)
            {
                _1023 = sampledArr[texelIdx_1] == 0u;
            }
            else
            {
                _1023 = _1016;
            }
            if (_1023)
            {
                uint nearest = texels;
                uint bestDist2 = 4294967295u;
                for (uint candidate = 0u; candidate < texels; candidate++)
                {
                    if (sampledArr[candidate] == 0u)
                    {
                        continue;
                    }
                    int dx = int(candidate % res) - int(texelIdx_1 % res);
                    int dy = int(candidate / res) - int(texelIdx_1 / res);
                    uint dist2 = uint((dx * dx) + (dy * dy));
                    if (dist2 < bestDist2)
                    {
                        bestDist2 = dist2;
                        nearest = candidate;
                    }
                }
                if (nearest < texels)
                {
                    if (rayCountArr[nearest] == 0u)
                    {
                        _1008.Store(meanIdx * 4 + 0, asuint(asfloat(_1100.Load((probeBase + nearest) * 4 + 0))));
                        _1008.Store(varIdx * 4 + 0, asuint(asfloat(_1100.Load(((probeBase + texels) + nearest) * 4 + 0))));
                    }
                    else
                    {
                        float mean = sumDist[nearest] / float(rayCountArr[nearest]);
                        float variance = abs((sumDistSq[nearest] / float(rayCountArr[nearest])) - (mean * mean));
                        if (rayCountArr[nearest] < 4u)
                        {
                            variance = max(variance, (volume_ProbeSpacing * volume_ProbeSpacing) * 0.100000001490116119384765625f);
                        }
                        _1008.Store(meanIdx * 4 + 0, asuint(mean));
                        _1008.Store(varIdx * 4 + 0, asuint(max(variance, 0.001000000047497451305389404296875f)));
                    }
                    continue;
                }
            }
            _1008.Store(meanIdx * 4 + 0, asuint(asfloat(_1100.Load(meanIdx * 4 + 0))));
            _1008.Store(varIdx * 4 + 0, asuint(asfloat(_1100.Load(varIdx * 4 + 0))));
            continue;
        }
        if (volume_RaySampleMode == 7)
        {
            float mean_1 = sumDist[texelIdx_1];
            float variance_1 = max(sumDistSq[texelIdx_1] / sumWeight[texelIdx_1], 0.0f);
            float oldMean = asfloat(_1100.Load(meanIdx * 4 + 0));
            float oldVariance = asfloat(_1100.Load(varIdx * 4 + 0));
            bool _1204 = oldMean >= 0.0f;
            bool _1210;
            if (_1204)
            {
                _1210 = !isnan(oldMean);
            }
            else
            {
                _1210 = _1204;
            }
            bool _1216;
            if (_1210)
            {
                _1216 = !isinf(oldMean);
            }
            else
            {
                _1216 = _1210;
            }
            bool _1219 = _1216 && (oldVariance >= 0.0f);
            bool _1225;
            if (_1219)
            {
                _1225 = !isnan(oldVariance);
            }
            else
            {
                _1225 = _1219;
            }
            bool _1231;
            if (_1225)
            {
                _1231 = !isinf(oldVariance);
            }
            else
            {
                _1231 = _1225;
            }
            bool historyValid = _1231;
            if (historyValid)
            {
                _1234 = lerp(oldMean, mean_1, alpha);
            }
            else
            {
                _1234 = mean_1;
            }
            _1008.Store(meanIdx * 4 + 0, asuint(_1234));
            if (historyValid)
            {
                _1247 = lerp(oldVariance, variance_1, alpha);
            }
            else
            {
                _1247 = variance_1;
            }
            _1008.Store(varIdx * 4 + 0, asuint(_1247));
            continue;
        }
        float mean_2 = sumDist[texelIdx_1] / float(rayCountArr[texelIdx_1]);
        float variance_2 = abs((sumDistSq[texelIdx_1] / float(rayCountArr[texelIdx_1])) - (mean_2 * mean_2));
        if (rayCountArr[texelIdx_1] < 4u)
        {
            variance_2 = max(variance_2, (volume_ProbeSpacing * volume_ProbeSpacing) * 0.100000001490116119384765625f);
        }
        variance_2 = max(variance_2, 0.001000000047497451305389404296875f);
        _1008.Store(meanIdx * 4 + 0, asuint(lerp(asfloat(_1100.Load(meanIdx * 4 + 0)), mean_2, alpha)));
        _1008.Store(varIdx * 4 + 0, asuint(lerp(asfloat(_1100.Load(varIdx * 4 + 0)), variance_2, alpha)));
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
