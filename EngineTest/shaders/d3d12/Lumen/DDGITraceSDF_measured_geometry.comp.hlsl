struct GeometryNode
{
    float4 lo;
    float4 hi;
    uint4 links;
};

struct GeometryInstance
{
    float4 inverse0;
    float4 inverse1;
    float4 inverse2;
    uint4 identity;
};

struct GeometryTriangle
{
    float4 a;
    float4 e1;
    float4 e2;
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

static const float3 _2403[14] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f), 0.57735025882720947265625f.xxx, float3(-0.57735025882720947265625f, 0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(-0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(-0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, -0.57735025882720947265625f), (-0.57735025882720947265625f).xxx };

ByteAddressBuffer geometryTop : register(t13, space0);
ByteAddressBuffer geometryInstances : register(t16, space0);
ByteAddressBuffer geometryBottom : register(t14, space0);
ByteAddressBuffer geometryTriangles : register(t15, space0);
cbuffer DDGIVolumeData : register(b3, space0)
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

cbuffer DetailSDFTable : register(b11, space0)
{
    float4 dtable_header : packoffset(c0);
    row_major float4x4 dtable_inv_world[4] : packoffset(c1);
    float4 dtable_normal_matrix[4][3] : packoffset(c17);
    float4 dtable_bounds_min[4] : packoffset(c29);
    float4 dtable_bounds_max[4] : packoffset(c33);
    float4 dtable_params[4] : packoffset(c37);
};

ByteAddressBuffer pul : register(t5, space0);
RWByteAddressBuffer robuf : register(u12, space0);
RWByteAddressBuffer hn : register(u6, space0);
RWByteAddressBuffer hd : register(u4, space0);
Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);
Texture3D<float4> detail_sdf_0 : register(t7, space0);
Texture3D<float4> detail_sdf_1 : register(t8, space0);
Texture3D<float4> detail_sdf_2 : register(t9, space0);
Texture3D<float4> detail_sdf_3 : register(t10, space0);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

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

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _1687 = local.x >= 0.0f;
    bool _1695;
    if (_1687)
    {
        _1695 = local.x < extent.x;
    }
    else
    {
        _1695 = _1687;
    }
    bool _1701;
    if (_1695)
    {
        _1701 = local.y >= 0.0f;
    }
    else
    {
        _1701 = _1695;
    }
    bool _1709;
    if (_1701)
    {
        _1709 = local.y < extent.y;
    }
    else
    {
        _1709 = _1701;
    }
    bool _1715;
    if (_1709)
    {
        _1715 = local.z >= 0.0f;
    }
    else
    {
        _1715 = _1709;
    }
    bool _1723;
    if (_1715)
    {
        _1723 = local.z < extent.z;
    }
    else
    {
        _1723 = _1715;
    }
    return _1723;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _1734 = uvw.x < 0.0f;
    bool _1741;
    if (!_1734)
    {
        _1741 = uvw.x > 1.0f;
    }
    else
    {
        _1741 = _1734;
    }
    bool _1748;
    if (!_1741)
    {
        _1748 = uvw.y < 0.0f;
    }
    else
    {
        _1748 = _1741;
    }
    bool _1755;
    if (!_1748)
    {
        _1755 = uvw.y > 1.0f;
    }
    else
    {
        _1755 = _1748;
    }
    bool _1762;
    if (!_1755)
    {
        _1762 = uvw.z < 0.0f;
    }
    else
    {
        _1762 = _1755;
    }
    bool _1769;
    if (!_1762)
    {
        _1769 = uvw.z > 1.0f;
    }
    else
    {
        _1769 = _1762;
    }
    if (_1769)
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
    bool _2250 = cc > 0u;
    bool _2264;
    if (_2250)
    {
        float3 param = pos;
        float3 param_1 = volume_SdfOrigins[0].xyz;
        float3 param_2 = volume_SdfExtents[0].xyz;
        _2264 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _2264 = _2250;
    }
    if (_2264)
    {
        float3 param_3 = pos;
        float3 param_4 = volume_SdfOrigins[0].xyz;
        float3 param_5 = volume_SdfExtents[0].xyz;
        uint param_6 = volume_SdfResolutionsAndCount.x;
        return sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    else
    {
        bool _2284 = cc > 1u;
        bool _2298;
        if (_2284)
        {
            float3 param_7 = pos;
            float3 param_8 = volume_SdfOrigins[1].xyz;
            float3 param_9 = volume_SdfExtents[1].xyz;
            _2298 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _2298 = _2284;
        }
        if (_2298)
        {
            float3 param_10 = pos;
            float3 param_11 = volume_SdfOrigins[1].xyz;
            float3 param_12 = volume_SdfExtents[1].xyz;
            uint param_13 = volume_SdfResolutionsAndCount.y;
            return sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13);
        }
        else
        {
            bool _2319 = cc > 2u;
            bool _2333;
            if (_2319)
            {
                float3 param_14 = pos;
                float3 param_15 = volume_SdfOrigins[2].xyz;
                float3 param_16 = volume_SdfExtents[2].xyz;
                _2333 = isInsideCascade(param_14, param_15, param_16);
            }
            else
            {
                _2333 = _2319;
            }
            if (_2333)
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

float3 ddgiRelocateProbeOrigin(float3 probePos, float spacing, out float outState)
{
    outState = 1.0f;
    float voxel = volume_SdfVoxelSizes[0].x;
    float skin = max(voxel * 1.5f, spacing * 0.0500000007450580596923828125f);
    float maxPush = spacing * 0.949999988079071044921875f;
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
            for (int st = 0; st < 10; st++)
            {
                p += (_2403[k] * stepLen);
                float3 delta = p - probePos;
                if (dot(delta, delta) > (maxPush * maxPush))
                {
                    break;
                }
                float3 param_1 = p;
                float d = sampleBestSDF(param_1);
                if ((d < 1000000000.0f) && (d > roundBestD))
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
    if (bestD <= skin)
    {
        float3 freeBest = best;
        float freeBestD = bestD;
        for (int dx = -1; dx <= 1; dx++)
        {
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int dz = -1; dz <= 1; dz++)
                {
                    if (((dx == 0) && (dy == 0)) && (dz == 0))
                    {
                        continue;
                    }
                    float3 c = probePos + (float3(float(dx), float(dy), float(dz)) * spacing);
                    float3 param_2 = c;
                    float d_1 = sampleBestSDF(param_2);
                    if ((d_1 < 1000000000.0f) && (d_1 > freeBestD))
                    {
                        freeBestD = d_1;
                        freeBest = c;
                    }
                }
            }
        }
        if (freeBestD > skin)
        {
            best = freeBest;
            outState = 0.5f;
        }
    }
    return best;
}

float3 ddgiDepthRayDirection(uint rayIndex)
{
    uint cell = rayIndex / 2u;
    float offset = ((rayIndex & 1u) == 0u) ? 0.25f : 0.75f;
    float2 uv = (((float2(float(cell % 8u), float(cell / 8u)) + offset.xx) / 8.0f.xx) * 2.0f) - 1.0f.xx;
    float3 d = float3(uv, (1.0f - abs(uv.x)) - abs(uv.y));
    if (d.z < 0.0f)
    {
        float3 _614 = d;
        float3 _619 = d;
        bool2 _621 = bool2(_619.xy.x >= 0.0f.xx.x, _619.xy.y >= 0.0f.xx.y);
        float2 _623 = (1.0f.xx - abs(_614.yx)) * float2(_621.x ? 1.0f.xx.x : (-1.0f).xx.x, _621.y ? 1.0f.xx.y : (-1.0f).xx.y);
        d.x = _623.x;
        d.y = _623.y;
    }
    return normalize(d);
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
    uint _203 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _208 = ddgiOctSampleHash(param_1);
    uint seed = _203 ^ _208;
    uint param_2 = seed ^ 1757159915u;
    uint _217 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _225 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_217 >> 14u), float(_225 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _278 = q;
        float3 _286 = q;
        bool2 _290 = bool2(_286.xy.x >= 0.0f.xx.x, _286.xy.y >= 0.0f.xx.y);
        float2 _292 = (1.0f.xx - abs(_278.yx)) * float2(_290.x ? 1.0f.xx.x : (-1.0f).xx.x, _290.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _292.x;
        q.y = _292.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _323 = rayCount >= 64u;
        bool _329;
        if (_323)
        {
            _329 = (rayCount % 64u) == 0u;
        }
        else
        {
            _329 = _323;
        }
        if (_329)
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

bool measuredGeometryReady(float3 origin, float3 direction, float maxDistance)
{
    bool _1493 = volume_SdfVoxelSizes[2].w != 1.0f;
    bool _1503;
    if (!_1493)
    {
        _1503 = volume_RaySampleMode != 7;
    }
    else
    {
        _1503 = _1493;
    }
    bool _1510;
    if (!_1503)
    {
        uint _1507;
        geometryTop.GetDimensions(_1507);
        _1507 = (_1507 - 0) / 48;
        _1510 = int(_1507) == 0;
    }
    else
    {
        _1510 = _1503;
    }
    bool _1517;
    if (!_1510)
    {
        uint _1514;
        geometryBottom.GetDimensions(_1514);
        _1514 = (_1514 - 0) / 48;
        _1517 = int(_1514) == 0;
    }
    else
    {
        _1517 = _1510;
    }
    bool _1524;
    if (!_1517)
    {
        uint _1521;
        geometryTriangles.GetDimensions(_1521);
        _1521 = (_1521 - 0) / 48;
        _1524 = int(_1521) == 0;
    }
    else
    {
        _1524 = _1517;
    }
    bool _1531;
    if (!_1524)
    {
        uint _1528;
        geometryInstances.GetDimensions(_1528);
        _1528 = (_1528 - 0) / 64;
        _1531 = int(_1528) == 0;
    }
    else
    {
        _1531 = _1524;
    }
    bool _1538;
    if (!_1531)
    {
        _1538 = any(isnan(origin));
    }
    else
    {
        _1538 = _1531;
    }
    bool _1545;
    if (!_1538)
    {
        _1545 = any(isinf(origin));
    }
    else
    {
        _1545 = _1538;
    }
    bool _1552;
    if (!_1545)
    {
        _1552 = any(isnan(direction));
    }
    else
    {
        _1552 = _1545;
    }
    bool _1559;
    if (!_1552)
    {
        _1559 = any(isinf(direction));
    }
    else
    {
        _1559 = _1552;
    }
    bool _1565;
    if (!_1559)
    {
        _1565 = isnan(maxDistance);
    }
    else
    {
        _1565 = _1559;
    }
    bool _1571;
    if (!_1565)
    {
        _1571 = isinf(maxDistance);
    }
    else
    {
        _1571 = _1565;
    }
    if (_1571 || (maxDistance <= 0.0f))
    {
        return false;
    }
    GeometryNode _1580;
    _1580.lo = asfloat(geometryTop.Load4(0));
    _1580.hi = asfloat(geometryTop.Load4(16));
    _1580.links = geometryTop.Load4(32);
    GeometryNode root;
    root.lo = _1580.lo;
    root.hi = _1580.hi;
    root.links = _1580.links;
    bool _1592 = !any(isnan(root.lo.xyz));
    bool _1601;
    if (_1592)
    {
        _1601 = !any(isinf(root.lo.xyz));
    }
    else
    {
        _1601 = _1592;
    }
    bool _1610;
    if (_1601)
    {
        _1610 = !any(isnan(root.hi.xyz));
    }
    else
    {
        _1610 = _1601;
    }
    bool _1619;
    if (_1610)
    {
        _1619 = !any(isinf(root.hi.xyz));
    }
    else
    {
        _1619 = _1610;
    }
    bool _1630;
    if (_1619)
    {
        _1630 = all(bool3(root.lo.xyz.x <= root.hi.xyz.x, root.lo.xyz.y <= root.hi.xyz.y, root.lo.xyz.z <= root.hi.xyz.z));
    }
    else
    {
        _1630 = _1619;
    }
    return _1630;
}

bool geometryBoxNonzero(GeometryNode n, float3 o, float3 d, inout float nearT, inout float farT)
{
    for (uint k = 0u; k < 3u; k++)
    {
        float a = (n.lo[k] - o[k]) / d[k];
        float b = (n.hi[k] - o[k]) / d[k];
        nearT = max(nearT, min(a, b));
        farT = min(farT, max(a, b));
        if (nearT > farT)
        {
            return false;
        }
    }
    return true;
}

bool geometryBox(GeometryNode n, float3 o, float3 d, inout float nearT, inout float farT)
{
    for (uint k = 0u; k < 3u; k++)
    {
        if (d[k] == 0.0f)
        {
            bool _716 = o[k] < n.lo[k];
            bool _728;
            if (!_716)
            {
                _728 = o[k] > n.hi[k];
            }
            else
            {
                _728 = _716;
            }
            if (_728)
            {
                return false;
            }
        }
        else
        {
            float a = (n.lo[k] - o[k]) / d[k];
            float b = (n.hi[k] - o[k]) / d[k];
            nearT = max(nearT, min(a, b));
            farT = min(farT, max(a, b));
            if (nearT > farT)
            {
                return false;
            }
        }
    }
    return true;
}

float geometryDifferenceOfProducts(float a, float b, float c, float d)
{
    precise float _635 = c * d;
    float cd = _635;
    float error = mad(-c, d, cd);
    float difference = mad(a, b, -cd);
    return difference + error;
}

float3 geometryTriangleNormal(float3 a, float3 b)
{
    float param = a.y;
    float param_1 = b.z;
    float param_2 = a.z;
    float param_3 = b.y;
    float param_4 = a.z;
    float param_5 = b.x;
    float param_6 = a.x;
    float param_7 = b.z;
    float param_8 = a.x;
    float param_9 = b.y;
    float param_10 = a.y;
    float param_11 = b.x;
    return float3(geometryDifferenceOfProducts(param, param_1, param_2, param_3), geometryDifferenceOfProducts(param_4, param_5, param_6, param_7), geometryDifferenceOfProducts(param_8, param_9, param_10, param_11));
}

int geometryFirstHit(float3 o, float3 d, float nearT, float farT, inout float bestT, inout uint owner, inout uint bestPrimitive, out float3 firstN, out float3 firstA, out float3 firstE1, out float3 firstE2)
{
    bool _840;
    if (false)
    {
        _840 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _840 = false;
    }
    bool _847;
    if (_840)
    {
        _847 = !any(isnan(d));
    }
    else
    {
        _847 = _840;
    }
    bool _854;
    if (_847)
    {
        _854 = !any(isinf(d));
    }
    else
    {
        _854 = _847;
    }
    bool topNonzero = _854;
    firstN = 0.0f.xxx;
    firstA = 0.0f.xxx;
    firstE1 = 0.0f.xxx;
    firstE2 = 0.0f.xxx;
    bestT = farT;
    owner = 0u;
    bestPrimitive = 4294967295u;
    bool found = false;
    uint topVisited = 0u;
    uint tp = 1u;
    uint ts[64];
    ts[0] = 0u;
    GeometryNode n;
    bool topHit;
    GeometryInstance inst;
    uint bs[64];
    GeometryNode b;
    bool bottomHit;
    GeometryTriangle tri;
    while (tp != 0u)
    {
        uint _871 = topVisited;
        topVisited = _871 + uint(1);
        uint _878;
        geometryTop.GetDimensions(_878);
        _878 = (_878 - 0) / 48;
        if (_871 >= uint(int(_878)))
        {
            return -1;
        }
        uint _887 = tp;
        uint _888 = _887 - uint(1);
        tp = _888;
        uint at = ts[_888];
        uint _892;
        geometryTop.GetDimensions(_892);
        _892 = (_892 - 0) / 48;
        if (at >= uint(int(_892)))
        {
            return -1;
        }
        GeometryNode _903;
        _903.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _903.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _903.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _903.lo;
        n.hi = _903.hi;
        n.links = _903.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _927 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _927;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _939 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _939;
        }
        if (!topHit)
        {
            continue;
        }
        if (n.links.w == 0u)
        {
            if ((tp + 2u) > 64u)
            {
                return -1;
            }
            uint _956 = tp;
            tp = _956 + uint(1);
            ts[_956] = n.links.x;
            uint _961 = tp;
            tp = _961 + uint(1);
            ts[_961] = n.links.y;
            continue;
        }
        uint _974;
        geometryInstances.GetDimensions(_974);
        _974 = (_974 - 0) / 64;
        if (n.links.z >= uint(int(_974)))
        {
            return -1;
        }
        GeometryInstance _988;
        _988.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64 + 0));
        _988.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _988.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _988.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _988.inverse0;
        inst.inverse1 = _988.inverse1;
        inst.inverse2 = _988.inverse2;
        inst.identity = _988.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        bool _1043 = any(isnan(lo));
        bool _1050;
        if (!_1043)
        {
            _1050 = any(isinf(lo));
        }
        else
        {
            _1050 = _1043;
        }
        bool _1057;
        if (!_1050)
        {
            _1057 = any(isnan(ld));
        }
        else
        {
            _1057 = _1050;
        }
        bool _1064;
        if (!_1057)
        {
            _1064 = any(isinf(ld));
        }
        else
        {
            _1064 = _1057;
        }
        if (_1064)
        {
            return -1;
        }
        bool _1074;
        if (false)
        {
            _1074 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _1074 = false;
        }
        bool bottomNonzero = _1074;
        uint bottomVisited = 0u;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _1088 = bottomVisited;
            bottomVisited = _1088 + uint(1);
            uint _1094;
            geometryBottom.GetDimensions(_1094);
            _1094 = (_1094 - 0) / 48;
            if (_1088 >= uint(int(_1094)))
            {
                return -1;
            }
            uint _1101 = bp;
            uint _1102 = _1101 - uint(1);
            bp = _1102;
            at = bs[_1102];
            uint _1106;
            geometryBottom.GetDimensions(_1106);
            _1106 = (_1106 - 0) / 48;
            if (at >= uint(int(_1106)))
            {
                return -1;
            }
            GeometryNode _1116;
            _1116.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _1116.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1116.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _1116.lo;
            b.hi = _1116.hi;
            b.links = _1116.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _1137 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _1137;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _1149 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _1149;
            }
            if (!bottomHit)
            {
                continue;
            }
            if (b.links.w == 0u)
            {
                if ((bp + 2u) > 64u)
                {
                    return -1;
                }
                uint _1166 = bp;
                bp = _1166 + uint(1);
                bs[_1166] = b.links.x;
                uint _1171 = bp;
                bp = _1171 + uint(1);
                bs[_1171] = b.links.y;
                continue;
            }
            uint _1184;
            geometryTriangles.GetDimensions(_1184);
            _1184 = (_1184 - 0) / 48;
            bool _1187 = b.links.z > uint(int(_1184));
            bool _1200;
            if (!_1187)
            {
                uint _1193;
                geometryTriangles.GetDimensions(_1193);
                _1193 = (_1193 - 0) / 48;
                _1200 = b.links.w > (uint(int(_1193)) - b.links.z);
            }
            else
            {
                _1200 = _1187;
            }
            if (_1200)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _1223;
                _1223.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 0));
                _1223.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _1223.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _1223.a;
                tri.e1 = _1223.e1;
                tri.e2 = _1223.e2;
                float3 p = cross(ld, tri.e2.xyz);
                float det = dot(tri.e1.xyz, p);
                if (det == 0.0f)
                {
                    continue;
                }
                float3 v = lo - tri.a.xyz;
                float u = dot(v, p) / det;
                if ((u < 0.0f) || (u > 1.0f))
                {
                    continue;
                }
                float3 q = cross(v, tri.e1.xyz);
                float bary = dot(ld, q) / det;
                float3 param_20 = tri.e1.xyz;
                float3 param_21 = tri.e2.xyz;
                float3 normal = geometryTriangleNormal(param_20, param_21);
                float planeDet = dot(ld, normal);
                if (planeDet == 0.0f)
                {
                    return -1;
                }
                float t = dot(tri.a.xyz - lo, normal) / planeDet;
                bool _1309 = isnan(t);
                bool _1315;
                if (!_1309)
                {
                    _1315 = isinf(t);
                }
                else
                {
                    _1315 = _1309;
                }
                bool _1321;
                if (!_1315)
                {
                    _1321 = isnan(bary);
                }
                else
                {
                    _1321 = _1315;
                }
                bool _1327;
                if (!_1321)
                {
                    _1327 = isinf(bary);
                }
                else
                {
                    _1327 = _1321;
                }
                bool _1333;
                if (!_1327)
                {
                    _1333 = isnan(u);
                }
                else
                {
                    _1333 = _1327;
                }
                bool _1339;
                if (!_1333)
                {
                    _1339 = isinf(u);
                }
                else
                {
                    _1339 = _1333;
                }
                if (_1339)
                {
                    return -1;
                }
                bool _1344 = bary < 0.0f;
                bool _1352;
                if (!_1344)
                {
                    _1352 = (u + bary) > 1.0f;
                }
                else
                {
                    _1352 = _1344;
                }
                bool _1356 = _1352 || (t < nearT);
                bool _1363;
                if (!_1356)
                {
                    _1363 = t > bestT;
                }
                else
                {
                    _1363 = _1356;
                }
                if (_1363)
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _1377;
                if (found)
                {
                    _1377 = t == bestT;
                }
                else
                {
                    _1377 = found;
                }
                bool _1398;
                if (_1377)
                {
                    bool _1383 = inst.identity.y > owner;
                    bool _1397;
                    if (!_1383)
                    {
                        bool _1390 = inst.identity.y == owner;
                        bool _1396;
                        if (_1390)
                        {
                            _1396 = primitive >= bestPrimitive;
                        }
                        else
                        {
                            _1396 = _1390;
                        }
                        _1397 = _1396;
                    }
                    else
                    {
                        _1397 = _1383;
                    }
                    _1398 = _1397;
                }
                else
                {
                    _1398 = _1377;
                }
                if (_1398)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
                float3 worldN = float3(dot(float3(inst.inverse0.x, inst.inverse1.x, inst.inverse2.x), normal), dot(float3(inst.inverse0.y, inst.inverse1.y, inst.inverse2.y), normal), dot(float3(inst.inverse0.z, inst.inverse1.z, inst.inverse2.z), normal));
                float normalScale = max(abs(worldN.x), max(abs(worldN.y), abs(worldN.z)));
                bool _1449 = any(isnan(worldN));
                bool _1456;
                if (!_1449)
                {
                    _1456 = any(isinf(worldN));
                }
                else
                {
                    _1456 = _1449;
                }
                if (_1456 || (normalScale <= 0.0f))
                {
                    return -1;
                }
                firstN = normalize(worldN / normalScale.xxx);
                firstA = tri.a.xyz;
                firstE1 = tri.e1.xyz;
                firstE2 = tri.e2.xyz;
            }
        }
    }
    return int(found);
}

void comp_main()
{
    uint tid = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    if (group_id >= volume_ProbeUpdateCount)
    {
        return;
    }
    bool _3379 = tid >= volume_RaysPerProbe;
    bool _3385;
    if (_3379)
    {
        _3385 = volume_RaySampleMode != 5;
    }
    else
    {
        _3385 = _3379;
    }
    if (_3385)
    {
        return;
    }
    uint probeIdx = pul.Load(group_id * 4 + 0);
    if (probeIdx >= volume_ProbeCountTotal)
    {
        return;
    }
    uint3 counts = volume_ProbeCounts.xyz;
    uint param = probeIdx;
    uint3 param_1 = counts;
    uint3 gc = ddgiProbeGridCoord(param, param_1);
    uint3 param_2 = gc;
    float3 param_3 = volume_ProbeOrigin.xyz;
    float param_4 = volume_ProbeSpacing;
    float3 probePos = ddgiProbeWorldPos(param_2, param_3, param_4);
    float originState = 1.0f;
    float3 param_5 = probePos;
    float param_6 = volume_ProbeSpacing;
    float param_7;
    float3 _3433 = ddgiRelocateProbeOrigin(param_5, param_6, param_7);
    originState = param_7;
    probePos = _3433;
    if (tid == 0u)
    {
        robuf.Store4(probeIdx * 16 + 0, asuint(float4(probePos, originState)));
    }
    float3 _3471;
    if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
    {
        _3471 = normalize(-volume_LightDirection.xyz);
    }
    else
    {
        _3471 = float3(0.0f, -1.0f, 0.0f);
    }
    float3 toLight = _3471;
    uint traceRayCount = volume_RaysPerProbe;
    float3 _3501;
    float param_21;
    uint param_22;
    uint param_23;
    float3 param_24;
    float3 param_25;
    float3 param_26;
    float3 param_27;
    GeometryNode param_28;
    for (uint rayId = tid; rayId < traceRayCount; rayId += 64u)
    {
        bool depthOnly = rayId >= volume_RaysPerProbe;
        if (depthOnly)
        {
            uint param_8 = rayId - volume_RaysPerProbe;
            _3501 = ddgiDepthRayDirection(param_8);
        }
        else
        {
            uint param_9 = rayId;
            uint param_10 = volume_RaysPerProbe;
            uint param_11 = volume_FrameIndex;
            uint param_12 = uint(volume_RaySampleMode);
            float3 param_13 = toLight;
            float4 _3526 = ddgiSampleRayDirection(param_9, param_10, param_11, param_12, param_13);
            _3501 = _3526.xyz;
        }
        float3 rayDir = _3501;
        bool _3530 = !depthOnly;
        bool _3537;
        if (_3530)
        {
            _3537 = volume_Albedo.w > 0.5f;
        }
        else
        {
            _3537 = _3530;
        }
        if (_3537)
        {
            uint sourceRayIdx = (group_id * volume_RaysPerProbe) + rayId;
            uint sourceBase = ((5u * volume_ProbeCountTotal) * volume_RaysPerProbe) + (4u * sourceRayIdx);
            hn.Store4((sourceBase + 0u) * 16 + 0, asuint(float4(probePos, asfloat(((volume_FrameIndex & 65535u) << 16u) | (probeIdx + 1u)))));
            hn.Store4((sourceBase + 1u) * 16 + 0, asuint(float4(rayDir, float(rayId + 1u))));
        }
        uint measuredRayIdx = (group_id * volume_RaysPerProbe) + rayId;
        float measuredDistance = -1.0f;
        float4 measuredNormal = 0.0f.xxxx;
        bool _3600 = !depthOnly;
        bool _3612;
        if (_3600)
        {
            float3 param_14 = probePos;
            float3 param_15 = rayDir;
            float param_16 = volume_RayMaxDistance;
            _3612 = measuredGeometryReady(param_14, param_15, param_16);
        }
        else
        {
            _3612 = _3600;
        }
        if (_3612)
        {
            float3 param_17 = probePos;
            float3 param_18 = rayDir;
            float param_19 = 0.0f;
            float param_20 = volume_RayMaxDistance;
            int _3638 = geometryFirstHit(param_17, param_18, param_19, param_20, param_21, param_22, param_23, param_24, param_25, param_26, param_27);
            float firstT = param_21;
            uint owner = param_22;
            uint primitive = param_23;
            float3 firstN = param_24;
            float3 firstA = param_25;
            float3 firstE1 = param_26;
            float3 firstE2 = param_27;
            int outcome = _3638;
            if (outcome == 1)
            {
                bool _3652 = !isnan(firstT);
                bool _3658;
                if (_3652)
                {
                    _3658 = !isinf(firstT);
                }
                else
                {
                    _3658 = _3652;
                }
                bool _3662 = _3658 && (firstT > 1.9999999494757503271102905273438e-05f);
                bool _3670;
                if (_3662)
                {
                    _3670 = firstT < (volume_RayMaxDistance - 1.9999999494757503271102905273438e-05f);
                }
                else
                {
                    _3670 = _3662;
                }
                bool _3673 = _3670 && (owner <= 65535u);
                bool _3680;
                if (_3673)
                {
                    _3680 = !any(isnan(firstN));
                }
                else
                {
                    _3680 = _3673;
                }
                bool _3687;
                if (_3680)
                {
                    _3687 = !any(isinf(firstN));
                }
                else
                {
                    _3687 = _3680;
                }
                bool _3694;
                if (_3687)
                {
                    _3694 = dot(firstN, firstN) > 0.25f;
                }
                else
                {
                    _3694 = _3687;
                }
                if (_3694)
                {
                    if (dot(firstN, rayDir) > 0.0f)
                    {
                        firstN = -firstN;
                    }
                    measuredDistance = firstT;
                    measuredNormal = float4(firstN, float(owner + 2u));
                }
            }
            else
            {
                if (outcome == 0)
                {
                    GeometryNode _3723;
                    _3723.lo = asfloat(geometryTop.Load4(0));
                    _3723.hi = asfloat(geometryTop.Load4(16));
                    _3723.links = geometryTop.Load4(32);
                    param_28.lo = _3723.lo;
                    param_28.hi = _3723.hi;
                    param_28.links = _3723.links;
                    float3 param_29 = probePos;
                    float3 param_30 = rayDir;
                    float param_31 = volume_RayMaxDistance;
                    float param_32 = 3.4028234663852885981170418348452e+38f;
                    bool _3738 = geometryBox(param_28, param_29, param_30, param_31, param_32);
                    bool sceneAhead = _3738;
                    measuredDistance = sceneAhead ? (-3.0f) : (-2.0f);
                }
            }
        }
        hn.Store4(measuredRayIdx * 16 + 0, asuint(measuredNormal));
        hd.Store(measuredRayIdx * 4 + 0, asuint(measuredDistance));
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
