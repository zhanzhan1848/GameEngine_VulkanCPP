struct SDFHitResult
{
    uint hit;
    float _distance;
    float3 position;
    float3 normal;
};

struct TetraResult
{
    uint pi[4];
    float bw[4];
};

struct DDGIRayData
{
    float4 radiance_and_dist;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

static const float3 _1858[14] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f), 0.57735025882720947265625f.xxx, float3(-0.57735025882720947265625f, 0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(-0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(-0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, -0.57735025882720947265625f), (-0.57735025882720947265625f).xxx };

cbuffer DDGIVolumeData : register(b5, space0)
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
    float4 volume_SkyColor : packoffset(c19);
    float4 volume_Albedo : packoffset(c20);
};

ByteAddressBuffer _1619 : register(t8, space0);
cbuffer GlobalShaderData : register(b4, space0)
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

ByteAddressBuffer _1989 : register(t7, space0);
RWByteAddressBuffer _2143 : register(u6, space0);
Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);
Texture2D<float4> gbuffer_albedo : register(t9, space0);
Texture2D<float4> prev_frame_color : register(t3, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

uint3 ddgiGetProbeCounts()
{
    return volume_ProbeCounts.xyz;
}

uint3 ddgiProbeGridCoord(uint probeIdx, uint3 counts)
{
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    return uint3(px, py, pz);
}

float3 ddgiProbeWorldPos(uint3 gc, float3 origin, float spacing)
{
    return origin + (float3(float(gc.x), float(gc.y), float(gc.z)) * spacing);
}

uint sdfCascadeCount()
{
    return volume_SdfResolutionsAndCount.w;
}

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _677 = local.x >= 0.0f;
    bool _685;
    if (_677)
    {
        _685 = local.x < extent.x;
    }
    else
    {
        _685 = _677;
    }
    bool _691;
    if (_685)
    {
        _691 = local.y >= 0.0f;
    }
    else
    {
        _691 = _685;
    }
    bool _699;
    if (_691)
    {
        _699 = local.y < extent.y;
    }
    else
    {
        _699 = _691;
    }
    bool _705;
    if (_699)
    {
        _705 = local.z >= 0.0f;
    }
    else
    {
        _705 = _699;
    }
    bool _713;
    if (_705)
    {
        _713 = local.z < extent.z;
    }
    else
    {
        _713 = _705;
    }
    return _713;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _585 = uvw.x < 0.0f;
    bool _592;
    if (!_585)
    {
        _592 = uvw.x > 1.0f;
    }
    else
    {
        _592 = _585;
    }
    bool _599;
    if (!_592)
    {
        _599 = uvw.y < 0.0f;
    }
    else
    {
        _599 = _592;
    }
    bool _606;
    if (!_599)
    {
        _606 = uvw.y > 1.0f;
    }
    else
    {
        _606 = _599;
    }
    bool _613;
    if (!_606)
    {
        _613 = uvw.z < 0.0f;
    }
    else
    {
        _613 = _606;
    }
    bool _620;
    if (!_613)
    {
        _620 = uvw.z > 1.0f;
    }
    else
    {
        _620 = _613;
    }
    if (_620)
    {
        return 10000000000.0f;
    }
    int resI = int(resolution);
    int tx = clamp(int(uvw.x * float(resI)), 0, resI - 1);
    int ty = clamp(int(uvw.y * float(resI)), 0, resI - 1);
    int tz = clamp(int(uvw.z * float(resI)), 0, resI - 1);
    return sdfTexture.Load(int4(int3(tx, ty, tz), 0)).x;
}

float sampleBestSDF(float3 pos)
{
    float d = 10000000000.0f;
    uint cc = sdfCascadeCount();
    bool _720 = cc > 0u;
    bool _737;
    if (_720)
    {
        float3 param = pos;
        float3 param_1 = volume_SdfOrigins[0].xyz;
        float3 param_2 = volume_SdfExtents[0].xyz;
        _737 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _737 = _720;
    }
    if (_737)
    {
        float3 param_3 = pos;
        float3 param_4 = volume_SdfOrigins[0].xyz;
        float3 param_5 = volume_SdfExtents[0].xyz;
        uint param_6 = volume_SdfResolutionsAndCount.x;
        d = min(d, sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6));
    }
    bool _758 = cc > 1u;
    bool _772;
    if (_758)
    {
        float3 param_7 = pos;
        float3 param_8 = volume_SdfOrigins[1].xyz;
        float3 param_9 = volume_SdfExtents[1].xyz;
        _772 = isInsideCascade(param_7, param_8, param_9);
    }
    else
    {
        _772 = _758;
    }
    if (_772)
    {
        float3 param_10 = pos;
        float3 param_11 = volume_SdfOrigins[1].xyz;
        float3 param_12 = volume_SdfExtents[1].xyz;
        uint param_13 = volume_SdfResolutionsAndCount.y;
        d = min(d, sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13));
    }
    bool _793 = cc > 2u;
    bool _808;
    if (_793)
    {
        float3 param_14 = pos;
        float3 param_15 = volume_SdfOrigins[2].xyz;
        float3 param_16 = volume_SdfExtents[2].xyz;
        _808 = isInsideCascade(param_14, param_15, param_16);
    }
    else
    {
        _808 = _793;
    }
    if (_808)
    {
        float3 param_17 = pos;
        float3 param_18 = volume_SdfOrigins[2].xyz;
        float3 param_19 = volume_SdfExtents[2].xyz;
        uint param_20 = volume_SdfResolutionsAndCount.z;
        d = min(d, sampleSDFCascade(sdf_cascade_2, param_17, param_18, param_19, param_20));
    }
    return d;
}

float3 ddgiRelocateProbeOrigin(float3 probePos, float spacing)
{
    float voxel = volume_SdfVoxelSizes[0].x;
    float skin = max(voxel * 1.5f, spacing * 0.0500000007450580596923828125f);
    float maxPush = spacing * 0.449999988079071044921875f;
    float3 param = probePos;
    float d0 = sampleBestSDF(param);
    if (d0 > skin)
    {
        return probePos;
    }
    float3 best = probePos;
    float bestD = d0;
    float stepLen = max(voxel, spacing * 0.100000001490116119384765625f);
    for (int _round = 0; _round < 3; _round++)
    {
        float3 roundBest = best;
        float roundBestD = bestD;
        for (int k = 0; k < 14; k++)
        {
            float3 p = best;
            for (int st = 0; st < 6; st++)
            {
                p += (_1858[k] * stepLen);
                float3 delta = p - probePos;
                if (dot(delta, delta) > (maxPush * maxPush))
                {
                    break;
                }
                float3 param_1 = p;
                float d = sampleBestSDF(param_1);
                if (d > roundBestD)
                {
                    roundBestD = d;
                    roundBest = p;
                }
                if (d > skin)
                {
                    break;
                }
            }
        }
        if (roundBestD <= bestD)
        {
            break;
        }
        best = roundBest;
        bestD = roundBestD;
        if (bestD > skin)
        {
            break;
        }
    }
    return best;
}

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
    uint _141 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _146 = ddgiOctSampleHash(param_1);
    uint seed = _141 ^ _146;
    uint param_2 = seed ^ 1757159915u;
    uint _155 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _163 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_155 >> 14u), float(_163 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _216 = q;
        float3 _224 = q;
        bool2 _228 = bool2(_224.xy.x >= 0.0f.xx.x, _224.xy.y >= 0.0f.xx.y);
        float2 _230 = (1.0f.xx - abs(_216.yx)) * float2(_228.x ? 1.0f.xx.x : (-1.0f).xx.x, _228.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _230.x;
        q.y = _230.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _261 = rayCount >= 64u;
        bool _267;
        if (_261)
        {
            _267 = (rayCount % 64u) == 0u;
        }
        else
        {
            _267 = _261;
        }
        if (_267)
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

SDFHitResult traceSDF(float3 rayOrigin, float3 rayDir, float maxDist)
{
    SDFHitResult result;
    result.hit = 0u;
    result.position = rayOrigin;
    result._distance = maxDist;
    result.normal = 0.0f.xxx;
    float t = 0.0f;
    uint cc = sdfCascadeCount();
    bool unsignedMode = cc == 1u;
    float voxelSize = volume_SdfVoxelSizes[0].x;
    float _855;
    if (unsignedMode)
    {
        _855 = voxelSize * 0.75f;
    }
    else
    {
        _855 = voxelSize * 0.5f;
    }
    float hitThreshold = _855;
    float3 _1071;
    for (uint _step = 0u; _step < 32u; _step++)
    {
        float3 pos = rayOrigin + (rayDir * t);
        float minDist = 10000000000.0f;
        bool insideAny = false;
        bool _884 = cc > 0u;
        bool _898;
        if (_884)
        {
            float3 param = pos;
            float3 param_1 = volume_SdfOrigins[0].xyz;
            float3 param_2 = volume_SdfExtents[0].xyz;
            _898 = isInsideCascade(param, param_1, param_2);
        }
        else
        {
            _898 = _884;
        }
        if (_898)
        {
            float3 param_3 = pos;
            float3 param_4 = volume_SdfOrigins[0].xyz;
            float3 param_5 = volume_SdfExtents[0].xyz;
            uint param_6 = volume_SdfResolutionsAndCount.x;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6));
            insideAny = true;
        }
        bool _919 = cc > 1u;
        bool _933;
        if (_919)
        {
            float3 param_7 = pos;
            float3 param_8 = volume_SdfOrigins[1].xyz;
            float3 param_9 = volume_SdfExtents[1].xyz;
            _933 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _933 = _919;
        }
        if (_933)
        {
            float3 param_10 = pos;
            float3 param_11 = volume_SdfOrigins[1].xyz;
            float3 param_12 = volume_SdfExtents[1].xyz;
            uint param_13 = volume_SdfResolutionsAndCount.y;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13));
            insideAny = true;
        }
        bool _953 = cc > 2u;
        bool _967;
        if (_953)
        {
            float3 param_14 = pos;
            float3 param_15 = volume_SdfOrigins[2].xyz;
            float3 param_16 = volume_SdfExtents[2].xyz;
            _967 = isInsideCascade(param_14, param_15, param_16);
        }
        else
        {
            _967 = _953;
        }
        if (_967)
        {
            float3 param_17 = pos;
            float3 param_18 = volume_SdfOrigins[2].xyz;
            float3 param_19 = volume_SdfExtents[2].xyz;
            uint param_20 = volume_SdfResolutionsAndCount.z;
            minDist = min(minDist, sampleSDFCascade(sdf_cascade_2, param_17, param_18, param_19, param_20));
            insideAny = true;
        }
        if (!insideAny)
        {
            uint coarsest = min(2u, (cc - 1u));
            t += (volume_SdfExtents[coarsest].x * 0.100000001490116119384765625f);
            if (t > maxDist)
            {
                break;
            }
            continue;
        }
        if (minDist < hitThreshold)
        {
            result.hit = 1u;
            result.position = pos;
            result._distance = t;
            float eps = max(voxelSize * 1.5f, 0.00999999977648258209228515625f);
            float3 param_21 = pos + float3(eps, 0.0f, 0.0f);
            float3 param_22 = pos - float3(eps, 0.0f, 0.0f);
            float3 param_23 = pos + float3(0.0f, eps, 0.0f);
            float3 param_24 = pos - float3(0.0f, eps, 0.0f);
            float3 param_25 = pos + float3(0.0f, 0.0f, eps);
            float3 param_26 = pos - float3(0.0f, 0.0f, eps);
            float3 gradient = float3(sampleBestSDF(param_21) - sampleBestSDF(param_22), sampleBestSDF(param_23) - sampleBestSDF(param_24), sampleBestSDF(param_25) - sampleBestSDF(param_26));
            float glen = length(gradient);
            if (glen > 9.9999999392252902907785028219223e-09f)
            {
                _1071 = gradient / glen.xxx;
            }
            else
            {
                _1071 = float3(0.0f, 1.0f, 0.0f);
            }
            result.normal = _1071;
            return result;
        }
        if (unsignedMode)
        {
            float _step_1 = min(minDist * 0.89999997615814208984375f, voxelSize * 4.0f);
            t += max(_step_1, voxelSize * 0.5f);
        }
        else
        {
            t += max(minDist, 0.00999999977648258209228515625f);
        }
        if (t > maxDist)
        {
            break;
        }
    }
    return result;
}

TetraResult tetrahedral(float3 gp, uint3 gd)
{
    uint3 b0 = clamp(uint3(floor(gp)), uint3(0u, 0u, 0u), gd - uint3(1u, 1u, 1u));
    uint3 b1 = min((b0 + uint3(1u, 1u, 1u)), (gd - uint3(1u, 1u, 1u)));
    uint gx = gd.x;
    uint gxy = gd.x * gd.y;
    uint p[8];
    p[0] = (b0.x + (b0.y * gx)) + (b0.z * gxy);
    p[1] = (b1.x + (b0.y * gx)) + (b0.z * gxy);
    p[2] = (b0.x + (b1.y * gx)) + (b0.z * gxy);
    p[3] = (b1.x + (b1.y * gx)) + (b0.z * gxy);
    p[4] = (b0.x + (b0.y * gx)) + (b1.z * gxy);
    p[5] = (b1.x + (b0.y * gx)) + (b1.z * gxy);
    p[6] = (b0.x + (b1.y * gx)) + (b1.z * gxy);
    p[7] = (b1.x + (b1.y * gx)) + (b1.z * gxy);
    float fx = frac(gp.x);
    float fy = frac(gp.y);
    float fz = frac(gp.z);
    TetraResult r;
    if ((fx >= fy) && (fy >= fz))
    {
        r.pi[0] = p[0];
        r.pi[1] = p[1];
        r.pi[2] = p[3];
        r.pi[3] = p[7];
        r.bw[0] = 1.0f - fx;
        r.bw[1] = fx - fy;
        r.bw[2] = fy - fz;
        r.bw[3] = fz;
    }
    else
    {
        if ((fx >= fz) && (fz >= fy))
        {
            r.pi[0] = p[0];
            r.pi[1] = p[1];
            r.pi[2] = p[5];
            r.pi[3] = p[7];
            r.bw[0] = 1.0f - fx;
            r.bw[1] = fx - fz;
            r.bw[2] = fz - fy;
            r.bw[3] = fy;
        }
        else
        {
            if ((fy >= fx) && (fx >= fz))
            {
                r.pi[0] = p[0];
                r.pi[1] = p[2];
                r.pi[2] = p[3];
                r.pi[3] = p[7];
                r.bw[0] = 1.0f - fy;
                r.bw[1] = fy - fx;
                r.bw[2] = fx - fz;
                r.bw[3] = fz;
            }
            else
            {
                if ((fy >= fz) && (fz >= fx))
                {
                    r.pi[0] = p[0];
                    r.pi[1] = p[2];
                    r.pi[2] = p[6];
                    r.pi[3] = p[7];
                    r.bw[0] = 1.0f - fy;
                    r.bw[1] = fy - fz;
                    r.bw[2] = fz - fx;
                    r.bw[3] = fx;
                }
                else
                {
                    if ((fz >= fx) && (fx >= fy))
                    {
                        r.pi[0] = p[0];
                        r.pi[1] = p[4];
                        r.pi[2] = p[5];
                        r.pi[3] = p[7];
                        r.bw[0] = 1.0f - fz;
                        r.bw[1] = fz - fx;
                        r.bw[2] = fx - fy;
                        r.bw[3] = fy;
                    }
                    else
                    {
                        r.pi[0] = p[0];
                        r.pi[1] = p[4];
                        r.pi[2] = p[6];
                        r.pi[3] = p[7];
                        r.bw[0] = 1.0f - fz;
                        r.bw[1] = fz - fy;
                        r.bw[2] = fy - fx;
                        r.bw[3] = fx;
                    }
                }
            }
        }
    }
    for (uint i = 0u; i < 4u; i++)
    {
        r.bw[i] = max(r.bw[i], 0.0f);
    }
    return r;
}

float3 shDot4(float3 c[4], float3 d)
{
    return (((c[0] * 0.2820949852466583251953125f) + (c[1] * ((-0.48860299587249755859375f) * d.y))) + (c[2] * (0.48860299587249755859375f * d.z))) + (c[3] * ((-0.48860299587249755859375f) * d.x));
}

float3 samplePrevProbeGrid(float3 pos, float3 N)
{
    if (volume_SkyColor.w == 1.0f)
    {
        return 0.0f.xxx;
    }
    bool diffuseHistory = volume_SkyColor.w == 2.0f;
    float3 origin = volume_ProbeOrigin.xyz;
    float spacing = volume_ProbeSpacing;
    uint3 counts = ddgiGetProbeCounts();
    float3 gp = (pos - origin) / spacing.xxx;
    float3 gridMax = float3(float(counts.x - 1u), float(counts.y - 1u), float(counts.z - 1u));
    bool _1559 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _1567;
    if (!_1559)
    {
        _1567 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _1567 = _1559;
    }
    if (_1567)
    {
        return 0.0f.xxx;
    }
    float3 param = gp;
    uint3 param_1 = counts;
    TetraResult tet = tetrahedral(param, param_1);
    float3 result = 0.0f.xxx;
    float totalWeight = 0.0f;
    float3 sh[4];
    for (uint p = 0u; p < 4u; p++)
    {
        if (tet.bw[p] < 0.001000000047497451305389404296875f)
        {
            continue;
        }
        uint base = tet.pi[p] * 36u;
        for (uint i = 0u; i < 4u; i++)
        {
            uint cb_i = base + (i * 4u);
            sh[i] = float3(asfloat(_1619.Load((cb_i + 0u) * 4 + 0)), asfloat(_1619.Load((cb_i + 1u) * 4 + 0)), asfloat(_1619.Load((cb_i + 2u) * 4 + 0)));
        }
        if (diffuseHistory)
        {
            for (uint i_1 = 1u; i_1 < 4u; i_1++)
            {
                sh[i_1] *= 0.666666686534881591796875f;
            }
        }
        float3 param_2[4] = sh;
        float3 param_3 = N;
        float3 irradiance = shDot4(param_2, param_3);
        result += (irradiance * tet.bw[p]);
        totalWeight += tet.bw[p];
    }
    if (totalWeight > 0.0f)
    {
        float3 value = result / totalWeight.xxx;
        float3 _1685;
        if (diffuseHistory)
        {
            _1685 = max(value, 0.0f.xxx);
        }
        else
        {
            _1685 = value;
        }
        return _1685;
    }
    return 0.0f.xxx;
}

float3 sampleHitAlbedo(float3 hitPos)
{
    float4 clip = mul(float4(hitPos, 1.0f), globalData_ViewProjection);
    if (clip.w <= 0.0f)
    {
        return volume_Albedo.xyz;
    }
    float3 ndc = clip.xyz / clip.w.xxx;
    bool _1731 = abs(ndc.x) > 1.0f;
    bool _1739;
    if (!_1731)
    {
        _1739 = abs(ndc.y) > 1.0f;
    }
    else
    {
        _1739 = _1731;
    }
    bool _1746;
    if (!_1739)
    {
        _1746 = ndc.z < 0.0f;
    }
    else
    {
        _1746 = _1739;
    }
    bool _1753;
    if (!_1746)
    {
        _1753 = ndc.z > 1.0f;
    }
    else
    {
        _1753 = _1746;
    }
    if (_1753)
    {
        return volume_Albedo.xyz;
    }
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, 1.0f - ((ndc.y * 0.5f) + 0.5f));
    uint _1778_dummy_parameter;
    int2 dims = int2(spvTextureSize(gbuffer_albedo, uint(0), _1778_dummy_parameter));
    int tx = clamp(int(uv.x * float(dims.x)), 0, dims.x - 1);
    int ty = clamp(int(uv.y * float(dims.y)), 0, dims.y - 1);
    return gbuffer_albedo.Load(int3(int2(tx, ty), 0)).xyz;
}

void comp_main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint totalRays = volume_ProbeUpdateCount * volume_RaysPerProbe;
    if (gid >= totalRays)
    {
        return;
    }
    uint localProbeIdx = gid / volume_RaysPerProbe;
    uint localRayIdx = gid % volume_RaysPerProbe;
    uint probeIdx = _1989.Load(localProbeIdx * 4 + 0);
    uint3 counts = ddgiGetProbeCounts();
    uint param = probeIdx;
    uint3 param_1 = counts;
    uint3 gc = ddgiProbeGridCoord(param, param_1);
    uint3 param_2 = gc;
    float3 param_3 = volume_ProbeOrigin.xyz;
    float param_4 = volume_ProbeSpacing;
    float3 probePos = ddgiProbeWorldPos(param_2, param_3, param_4);
    float3 param_5 = probePos;
    float param_6 = volume_ProbeSpacing;
    probePos = ddgiRelocateProbeOrigin(param_5, param_6);
    float3 _2038;
    if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
    {
        _2038 = normalize(-volume_LightDirection.xyz);
    }
    else
    {
        _2038 = float3(0.0f, -1.0f, 0.0f);
    }
    float3 toLight = _2038;
    uint param_7 = localRayIdx;
    uint param_8 = volume_RaysPerProbe;
    uint param_9 = volume_FrameIndex;
    uint param_10 = uint(volume_RaySampleMode);
    float3 param_11 = toLight;
    float4 _2066 = ddgiSampleRayDirection(param_7, param_8, param_9, param_10, param_11);
    float3 rayDir = _2066.xyz;
    float3 param_12 = probePos;
    float3 param_13 = rayDir;
    float param_14 = volume_RayMaxDistance;
    SDFHitResult hit = traceSDF(param_12, param_13, param_14);
    DDGIRayData result;
    if (hit.hit != 0u)
    {
        float3 N = hit.normal;
        float NdotUp = max(0.0f, N.y);
        float3 E_sky = volume_SkyColor.xyz * (0.5f + (0.5f * NdotUp));
        float3 param_15 = hit.position;
        float3 param_16 = N;
        float3 L_i_prev = samplePrevProbeGrid(param_15, param_16);
        float3 param_17 = hit.position;
        float3 albedo = sampleHitAlbedo(param_17);
        float3 L_out = (albedo / 3.1415927410125732421875f.xxx) * (E_sky + (L_i_prev * 3.1415927410125732421875f));
        result.radiance_and_dist = float4(L_out, hit._distance);
    }
    else
    {
        result.radiance_and_dist = float4(volume_SkyColor.xyz, -1.0f);
    }
    _2143.Store4(((probeIdx * volume_RaysPerProbe) + localRayIdx) * 16 + 0, asuint(result.radiance_and_dist));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
