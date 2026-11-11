static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

static const float3 _1368[14] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f), 0.57735025882720947265625f.xxx, float3(-0.57735025882720947265625f, 0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(-0.57735025882720947265625f, -0.57735025882720947265625f, 0.57735025882720947265625f), float3(0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(-0.57735025882720947265625f, 0.57735025882720947265625f, -0.57735025882720947265625f), float3(0.57735025882720947265625f, -0.57735025882720947265625f, -0.57735025882720947265625f), (-0.57735025882720947265625f).xxx };

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
    bool _635 = local.x >= 0.0f;
    bool _643;
    if (_635)
    {
        _643 = local.x < extent.x;
    }
    else
    {
        _643 = _635;
    }
    bool _649;
    if (_643)
    {
        _649 = local.y >= 0.0f;
    }
    else
    {
        _649 = _643;
    }
    bool _657;
    if (_649)
    {
        _657 = local.y < extent.y;
    }
    else
    {
        _657 = _649;
    }
    bool _663;
    if (_657)
    {
        _663 = local.z >= 0.0f;
    }
    else
    {
        _663 = _657;
    }
    bool _671;
    if (_663)
    {
        _671 = local.z < extent.z;
    }
    else
    {
        _671 = _663;
    }
    return _671;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent, uint resolution)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _682 = uvw.x < 0.0f;
    bool _689;
    if (!_682)
    {
        _689 = uvw.x > 1.0f;
    }
    else
    {
        _689 = _682;
    }
    bool _696;
    if (!_689)
    {
        _696 = uvw.y < 0.0f;
    }
    else
    {
        _696 = _689;
    }
    bool _703;
    if (!_696)
    {
        _703 = uvw.y > 1.0f;
    }
    else
    {
        _703 = _696;
    }
    bool _710;
    if (!_703)
    {
        _710 = uvw.z < 0.0f;
    }
    else
    {
        _710 = _703;
    }
    bool _717;
    if (!_710)
    {
        _717 = uvw.z > 1.0f;
    }
    else
    {
        _717 = _710;
    }
    if (_717)
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
    bool _1213 = cc > 0u;
    bool _1227;
    if (_1213)
    {
        float3 param = pos;
        float3 param_1 = volume_SdfOrigins[0].xyz;
        float3 param_2 = volume_SdfExtents[0].xyz;
        _1227 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _1227 = _1213;
    }
    if (_1227)
    {
        float3 param_3 = pos;
        float3 param_4 = volume_SdfOrigins[0].xyz;
        float3 param_5 = volume_SdfExtents[0].xyz;
        uint param_6 = volume_SdfResolutionsAndCount.x;
        return sampleSDFCascade(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    else
    {
        bool _1247 = cc > 1u;
        bool _1261;
        if (_1247)
        {
            float3 param_7 = pos;
            float3 param_8 = volume_SdfOrigins[1].xyz;
            float3 param_9 = volume_SdfExtents[1].xyz;
            _1261 = isInsideCascade(param_7, param_8, param_9);
        }
        else
        {
            _1261 = _1247;
        }
        if (_1261)
        {
            float3 param_10 = pos;
            float3 param_11 = volume_SdfOrigins[1].xyz;
            float3 param_12 = volume_SdfExtents[1].xyz;
            uint param_13 = volume_SdfResolutionsAndCount.y;
            return sampleSDFCascade(sdf_cascade_1, param_10, param_11, param_12, param_13);
        }
        else
        {
            bool _1282 = cc > 2u;
            bool _1296;
            if (_1282)
            {
                float3 param_14 = pos;
                float3 param_15 = volume_SdfOrigins[2].xyz;
                float3 param_16 = volume_SdfExtents[2].xyz;
                _1296 = isInsideCascade(param_14, param_15, param_16);
            }
            else
            {
                _1296 = _1282;
            }
            if (_1296)
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
                p += (_1368[k] * stepLen);
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
        float3 _563 = d;
        float3 _568 = d;
        bool2 _570 = bool2(_568.xy.x >= 0.0f.xx.x, _568.xy.y >= 0.0f.xx.y);
        float2 _572 = (1.0f.xx - abs(_563.yx)) * float2(_570.x ? 1.0f.xx.x : (-1.0f).xx.x, _570.y ? 1.0f.xx.y : (-1.0f).xx.y);
        d.x = _572.x;
        d.y = _572.y;
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
    uint _152 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _157 = ddgiOctSampleHash(param_1);
    uint seed = _152 ^ _157;
    uint param_2 = seed ^ 1757159915u;
    uint _166 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _174 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_166 >> 14u), float(_174 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _227 = q;
        float3 _235 = q;
        bool2 _239 = bool2(_235.xy.x >= 0.0f.xx.x, _235.xy.y >= 0.0f.xx.y);
        float2 _241 = (1.0f.xx - abs(_227.yx)) * float2(_239.x ? 1.0f.xx.x : (-1.0f).xx.x, _239.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _241.x;
        q.y = _241.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _272 = rayCount >= 64u;
        bool _278;
        if (_272)
        {
            _278 = (rayCount % 64u) == 0u;
        }
        else
        {
            _278 = _272;
        }
        if (_278)
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

float sampleHitEvidence(float3 pos)
{
    float3 uv = (pos - volume_SdfOrigins[0].xyz) / volume_SdfExtents[0].xyz;
    bool _903 = any(bool3(uv.x < 0.0f.xxx.x, uv.y < 0.0f.xxx.y, uv.z < 0.0f.xxx.z));
    bool _911;
    if (!_903)
    {
        _911 = any(bool3(uv.x > 1.0f.xxx.x, uv.y > 1.0f.xxx.y, uv.z > 1.0f.xxx.z));
    }
    else
    {
        _911 = _903;
    }
    if (_911)
    {
        return 10000000000.0f;
    }
    uint res = volume_SdfResolutionsAndCount.x;
    float3 p = (uv * float(res)) - 0.5f.xxx;
    float3 f = frac(p);
    int3 base = int3(floor(p));
    float value = 0.0f;
    for (int c = 0; c < 8; c++)
    {
        int3 off = int3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        float3 w = lerp(1.0f.xxx - f, f, float3(off));
        float weight = (w.x * w.y) * w.z;
        if (weight <= 1.0000000116860974230803549289703e-07f)
        {
            continue;
        }
        float d = sdf_cascade_0.Load(int4(clamp(base + off, int3(0, 0, 0), (int(res) - 1).xxx), 0)).y;
        if (d >= 65000.0f)
        {
            return 10000000000.0f;
        }
        value += (weight * d);
    }
    return value;
}

float detailSampleRaw(Texture3D<float4> tex, float3 bounds_min, float3 extent, float res, float3 local)
{
    float3 uvw = (local - bounds_min) / max(extent, 9.9999999747524270787835121154785e-07f.xxx);
    bool _1567 = uvw.x < 0.0f;
    bool _1574;
    if (!_1567)
    {
        _1574 = uvw.x > 1.0f;
    }
    else
    {
        _1574 = _1567;
    }
    bool _1581;
    if (!_1574)
    {
        _1581 = uvw.y < 0.0f;
    }
    else
    {
        _1581 = _1574;
    }
    bool _1588;
    if (!_1581)
    {
        _1588 = uvw.y > 1.0f;
    }
    else
    {
        _1588 = _1581;
    }
    bool _1595;
    if (!_1588)
    {
        _1595 = uvw.z < 0.0f;
    }
    else
    {
        _1595 = _1588;
    }
    bool _1602;
    if (!_1595)
    {
        _1602 = uvw.z > 1.0f;
    }
    else
    {
        _1602 = _1595;
    }
    if (_1602)
    {
        return 1.0f;
    }
    float3 p = (uvw * res) - 0.5f.xxx;
    int3 base = int3(floor(p));
    float3 f = p - float3(base);
    int3 resI = (int(res) - 1).xxx;
    int3 c0 = clamp(base, int3(0, 0, 0), resI);
    int3 c1 = clamp(base + int3(1, 1, 1), int3(0, 0, 0), resI);
    float d000 = tex.Load(int4(int3(c0.x, c0.y, c0.z), 0)).x;
    float d100 = tex.Load(int4(int3(c1.x, c0.y, c0.z), 0)).x;
    float d010 = tex.Load(int4(int3(c0.x, c1.y, c0.z), 0)).x;
    float d110 = tex.Load(int4(int3(c1.x, c1.y, c0.z), 0)).x;
    float d001 = tex.Load(int4(int3(c0.x, c0.y, c1.z), 0)).x;
    float d101 = tex.Load(int4(int3(c1.x, c0.y, c1.z), 0)).x;
    float d011 = tex.Load(int4(int3(c0.x, c1.y, c1.z), 0)).x;
    float d111 = tex.Load(int4(int3(c1.x, c1.y, c1.z), 0)).x;
    return lerp(lerp(lerp(d000, d100, f.x), lerp(d010, d110, f.x), f.y), lerp(lerp(d001, d101, f.x), lerp(d011, d111, f.x), f.y), f.z);
}

float detailSampleK(uint k, float3 local)
{
    float3 bmin = dtable_bounds_min[k].xyz;
    float3 ext = dtable_bounds_max[k].xyz - bmin;
    float res = dtable_params[k].x;
    if (k == 0u)
    {
        float3 param = bmin;
        float3 param_1 = ext;
        float param_2 = res;
        float3 param_3 = local;
        return detailSampleRaw(detail_sdf_0, param, param_1, param_2, param_3);
    }
    if (k == 1u)
    {
        float3 param_4 = bmin;
        float3 param_5 = ext;
        float param_6 = res;
        float3 param_7 = local;
        return detailSampleRaw(detail_sdf_1, param_4, param_5, param_6, param_7);
    }
    if (k == 2u)
    {
        float3 param_8 = bmin;
        float3 param_9 = ext;
        float param_10 = res;
        float3 param_11 = local;
        return detailSampleRaw(detail_sdf_2, param_8, param_9, param_10, param_11);
    }
    float3 param_12 = bmin;
    float3 param_13 = ext;
    float param_14 = res;
    float3 param_15 = local;
    return detailSampleRaw(detail_sdf_3, param_12, param_13, param_14, param_15);
}

float detailDecode(uint k, float raw)
{
    float max_dim = dtable_params[k].y;
    if (dtable_params[k].z > 0.5f)
    {
        return ((raw * 2.0f) * max_dim) - max_dim;
    }
    return raw * max_dim;
}

bool ddgiDetailRefine(float3 probePos, float3 rayDir, inout float t, out float3 outN)
{
    outN = 0.0f.xxx;
    uint count = uint(dtable_header.x);
    if (count == 0u)
    {
        return false;
    }
    float3 hitPos = probePos + (rayDir * t);
    int best = -1;
    float bestAbs = 3.3999999521443642490773241379936e+38f;
    float3 bestLocal = 0.0f.xxx;
    for (uint k = 0u; k < count; k++)
    {
        bool _1895 = any(bool3(hitPos.x < dtable_bounds_min[k].xyz.x, hitPos.y < dtable_bounds_min[k].xyz.y, hitPos.z < dtable_bounds_min[k].xyz.z));
        bool _1906;
        if (!_1895)
        {
            _1906 = any(bool3(hitPos.x > dtable_bounds_max[k].xyz.x, hitPos.y > dtable_bounds_max[k].xyz.y, hitPos.z > dtable_bounds_max[k].xyz.z));
        }
        else
        {
            _1906 = _1895;
        }
        if (_1906)
        {
            continue;
        }
        float3 local = mul(float4(hitPos, 1.0f), dtable_inv_world[k]).xyz;
        uint param = k;
        float3 param_1 = local;
        uint param_2 = k;
        float param_3 = detailSampleK(param, param_1);
        float d = detailDecode(param_2, param_3);
        float a = abs(d);
        if (a < bestAbs)
        {
            bestAbs = a;
            best = int(k);
            bestLocal = local;
        }
    }
    if (best < 0)
    {
        return false;
    }
    uint bk = uint(best);
    float localVoxel = max(dtable_params[bk].w, 9.9999997473787516355514526367188e-06f);
    bool signedField = dtable_params[bk].z > 0.5f;
    float fBest = bestAbs;
    float tBest = t;
    if (signedField)
    {
        float tLo = t;
        float tHi = t;
        float fLo = bestAbs;
        float fHi = bestAbs;
        bool haveLo = false;
        bool haveHi = false;
        for (int i = -5; i <= 5; i++)
        {
            float ti = t + (float(i) * localVoxel);
            float3 lp = mul(float4(probePos + (rayDir * ti), 1.0f), dtable_inv_world[bk]).xyz;
            uint param_4 = bk;
            float3 param_5 = lp;
            uint param_6 = bk;
            float param_7 = detailSampleK(param_4, param_5);
            float fi = detailDecode(param_6, param_7);
            bool _2024 = fi < 0.0f;
            bool _2033;
            if (_2024)
            {
                _2033 = (!haveLo) || (ti > tLo);
            }
            else
            {
                _2033 = _2024;
            }
            if (_2033)
            {
                tLo = ti;
                fLo = fi;
                haveLo = true;
            }
            bool _2039 = fi >= 0.0f;
            bool _2048;
            if (_2039)
            {
                _2048 = (!haveHi) || (ti < tHi);
            }
            else
            {
                _2048 = _2039;
            }
            if (_2048)
            {
                tHi = ti;
                fHi = fi;
                haveHi = true;
            }
            if (abs(fi) < fBest)
            {
                fBest = abs(fi);
                tBest = ti;
            }
        }
        if (haveLo && haveHi)
        {
            for (int it = 0; it < 5; it++)
            {
                float tm = 0.5f * (tLo + tHi);
                float3 lp_1 = mul(float4(probePos + (rayDir * tm), 1.0f), dtable_inv_world[bk]).xyz;
                uint param_8 = bk;
                float3 param_9 = lp_1;
                uint param_10 = bk;
                float param_11 = detailSampleK(param_8, param_9);
                float fm = detailDecode(param_10, param_11);
                if (fm < 0.0f)
                {
                    tLo = tm;
                }
                else
                {
                    tHi = tm;
                }
            }
            tBest = 0.5f * (tLo + tHi);
        }
    }
    else
    {
        for (int i_1 = 1; i_1 <= 6; i_1++)
        {
            float ti_1 = t + ((float(i_1) * 0.5f) * localVoxel);
            float3 lp_2 = mul(float4(probePos + (rayDir * ti_1), 1.0f), dtable_inv_world[bk]).xyz;
            uint param_12 = bk;
            float3 param_13 = lp_2;
            uint param_14 = bk;
            float param_15 = detailSampleK(param_12, param_13);
            float fi_1 = detailDecode(param_14, param_15);
            if (abs(fi_1) < fBest)
            {
                fBest = abs(fi_1);
                tBest = ti_1;
            }
            else
            {
                break;
            }
        }
    }
    float e = 0.75f * localVoxel;
    float3 lp_3 = mul(float4(probePos + (rayDir * tBest), 1.0f), dtable_inv_world[bk]).xyz;
    uint param_16 = bk;
    float3 param_17 = lp_3 + float3(e, 0.0f, 0.0f);
    uint param_18 = bk;
    float param_19 = detailSampleK(param_16, param_17);
    uint param_20 = bk;
    float3 param_21 = lp_3 - float3(e, 0.0f, 0.0f);
    uint param_22 = bk;
    float param_23 = detailSampleK(param_20, param_21);
    float dx = detailDecode(param_18, param_19) - detailDecode(param_22, param_23);
    uint param_24 = bk;
    float3 param_25 = lp_3 + float3(0.0f, e, 0.0f);
    uint param_26 = bk;
    float param_27 = detailSampleK(param_24, param_25);
    uint param_28 = bk;
    float3 param_29 = lp_3 - float3(0.0f, e, 0.0f);
    uint param_30 = bk;
    float param_31 = detailSampleK(param_28, param_29);
    float dy = detailDecode(param_26, param_27) - detailDecode(param_30, param_31);
    uint param_32 = bk;
    float3 param_33 = lp_3 + float3(0.0f, 0.0f, e);
    uint param_34 = bk;
    float param_35 = detailSampleK(param_32, param_33);
    uint param_36 = bk;
    float3 param_37 = lp_3 - float3(0.0f, 0.0f, e);
    uint param_38 = bk;
    float param_39 = detailSampleK(param_36, param_37);
    float dz = detailDecode(param_34, param_35) - detailDecode(param_38, param_39);
    float3 gLocal = float3(dx, dy, dz);
    if (dot(gLocal, gLocal) < 9.9999998245167004418121337039338e-15f)
    {
        return false;
    }
    float3 n = normalize(((dtable_normal_matrix[bk][0].xyz * gLocal.x) + (dtable_normal_matrix[bk][1].xyz * gLocal.y)) + (dtable_normal_matrix[bk][2].xyz * gLocal.z));
    if (dot(n, rayDir) > 0.0f)
    {
        n = -n;
    }
    outN = n;
    t = tBest;
    return true;
}

bool evidenceRawNormal(float3 pos, inout float3 n)
{
    float3 h = volume_SdfExtents[0].xyz / float(volume_SdfResolutionsAndCount.x).xxx;
    float3 param = pos + float3(h.x, 0.0f, 0.0f);
    float3 param_1 = pos + float3(0.0f, h.y, 0.0f);
    float3 param_2 = pos + float3(0.0f, 0.0f, h.z);
    float3 plus = float3(sampleHitEvidence(param), sampleHitEvidence(param_1), sampleHitEvidence(param_2));
    float3 param_3 = pos - float3(h.x, 0.0f, 0.0f);
    float3 param_4 = pos - float3(0.0f, h.y, 0.0f);
    float3 param_5 = pos - float3(0.0f, 0.0f, h.z);
    float3 minus = float3(sampleHitEvidence(param_3), sampleHitEvidence(param_4), sampleHitEvidence(param_5));
    bool _1063 = any(isnan(plus));
    bool _1070;
    if (!_1063)
    {
        _1070 = any(isinf(plus));
    }
    else
    {
        _1070 = _1063;
    }
    bool _1077;
    if (!_1070)
    {
        _1077 = any(isnan(minus));
    }
    else
    {
        _1077 = _1070;
    }
    bool _1084;
    if (!_1077)
    {
        _1084 = any(isinf(minus));
    }
    else
    {
        _1084 = _1077;
    }
    bool _1092;
    if (!_1084)
    {
        _1092 = any(bool3(plus.x >= 65000.0f.xxx.x, plus.y >= 65000.0f.xxx.y, plus.z >= 65000.0f.xxx.z));
    }
    else
    {
        _1092 = _1084;
    }
    bool _1099;
    if (!_1092)
    {
        _1099 = any(bool3(minus.x >= 65000.0f.xxx.x, minus.y >= 65000.0f.xxx.y, minus.z >= 65000.0f.xxx.z));
    }
    else
    {
        _1099 = _1092;
    }
    if (_1099)
    {
        return false;
    }
    float3 g = (plus - minus) / (h * 2.0f);
    if (dot(g, g) < 9.9999999392252902907785028219223e-09f)
    {
        return false;
    }
    n = normalize(g);
    return true;
}

bool evidenceHitNormal(float3 pos, float3 rayDir, inout float3 n)
{
    float3 param = pos;
    float3 param_1;
    bool _1127 = evidenceRawNormal(param, param_1);
    n = param_1;
    if (!_1127)
    {
        return false;
    }
    if (dot(n, rayDir) > 0.0f)
    {
        n = -n;
    }
    return true;
}

bool evidenceMaterialPoint(float3 pos, float band, inout float3 projected)
{
    float3 param = pos;
    float d = sampleHitEvidence(param);
    bool _1148 = isnan(d);
    bool _1154;
    if (!_1148)
    {
        _1154 = isinf(d);
    }
    else
    {
        _1154 = _1148;
    }
    bool _1161 = (_1154 || (d < 0.0f)) || (d > band);
    float3 rawN;
    bool _1172;
    if (!_1161)
    {
        float3 param_1 = pos;
        float3 param_2;
        bool _1169 = evidenceRawNormal(param_1, param_2);
        rawN = param_2;
        _1172 = !_1169;
    }
    else
    {
        _1172 = _1161;
    }
    if (_1172)
    {
        return false;
    }
    projected = pos - (rawN * d);
    float3 param_3 = projected;
    float residual = sampleHitEvidence(param_3);
    bool _1187 = !isnan(residual);
    bool _1193;
    if (_1187)
    {
        _1193 = !isinf(residual);
    }
    else
    {
        _1193 = _1187;
    }
    bool _1196 = _1193 && (residual >= 0.0f);
    bool _1206;
    if (_1196)
    {
        _1206 = (residual < d) || (d < 9.9999997473787516355514526367188e-05f);
    }
    else
    {
        _1206 = _1196;
    }
    return _1206;
}

void comp_main()
{
    uint tid = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    if (group_id >= volume_ProbeUpdateCount)
    {
        return;
    }
    bool _2347 = tid >= volume_RaysPerProbe;
    bool _2355;
    if (_2347)
    {
        _2355 = volume_RaySampleMode != 5;
    }
    else
    {
        _2355 = _2347;
    }
    if (_2355)
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
    float3 _2404 = ddgiRelocateProbeOrigin(param_5, param_6, param_7);
    originState = param_7;
    probePos = _2404;
    if (tid == 0u)
    {
        robuf.Store4(probeIdx * 16 + 0, asuint(float4(probePos, originState)));
    }
    float3 _2442;
    if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
    {
        _2442 = normalize(-volume_LightDirection.xyz);
    }
    else
    {
        _2442 = float3(0.0f, -1.0f, 0.0f);
    }
    float3 toLight = _2442;
    uint traceRayCount = volume_RaysPerProbe + ((volume_RaySampleMode == 5) ? 128u : 0u);
    float3 _2478;
    uint _2597;
    float _2643;
    float _2709;
    float advance;
    float _2793;
    float3 param_21;
    float3 param_24;
    float3 projected;
    float3 param_27;
    float4 _2955;
    for (uint rayId = tid; rayId < traceRayCount; rayId += 64u)
    {
        bool depthOnly = rayId >= volume_RaysPerProbe;
        if (depthOnly)
        {
            uint param_8 = rayId - volume_RaysPerProbe;
            _2478 = ddgiDepthRayDirection(param_8);
        }
        else
        {
            uint param_9 = rayId;
            uint param_10 = volume_RaysPerProbe;
            uint param_11 = volume_FrameIndex;
            uint param_12 = uint(volume_RaySampleMode);
            float3 param_13 = toLight;
            float4 _2503 = ddgiSampleRayDirection(param_9, param_10, param_11, param_12, param_13);
            _2478 = _2503.xyz;
        }
        float3 rayDir = _2478;
        bool _2507 = !depthOnly;
        bool _2514;
        if (_2507)
        {
            _2514 = volume_Albedo.w > 0.5f;
        }
        else
        {
            _2514 = _2507;
        }
        if (_2514)
        {
            uint sourceRayIdx = (group_id * volume_RaysPerProbe) + rayId;
            uint sourceBase = ((5u * volume_ProbeCountTotal) * volume_RaysPerProbe) + (4u * sourceRayIdx);
            hn.Store4((sourceBase + 0u) * 16 + 0, asuint(float4(probePos, asfloat(((volume_FrameIndex & 65535u) << 16u) | (probeIdx + 1u)))));
            hn.Store4((sourceBase + 1u) * 16 + 0, asuint(float4(rayDir, float(rayId + 1u))));
        }
        float t = 0.0f;
        float hitDistance = -1.0f;
        float hitThreshold = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
        float minStep = volume_SdfVoxelSizes[0].x * 0.25f;
        uint coarsest = min(2u, (volume_SdfResolutionsAndCount.w - 1u));
        float fallbackStride = volume_SdfExtents[coarsest].x * 0.100000001490116119384765625f;
        bool _2596;
        if (depthOnly)
        {
            _2596 = volume_SdfVoxelSizes[1].w > 0.0f;
        }
        else
        {
            _2596 = depthOnly;
        }
        if (_2596)
        {
            _2597 = uint(volume_SdfVoxelSizes[1].w);
        }
        else
        {
            _2597 = uint(volume_SdfVoxelSizes[0].y);
        }
        uint evidenceMode = _2597;
        uint MAX_SDF_STEPS = (evidenceMode == 3u) ? 256u : 64u;
        float prevD = 3.3999999521443642490773241379936e+38f;
        bool clearedSkin = false;
        bool signedCoarseField = volume_ProbeCounts.w != 0u;
        float prevT = t;
        for (uint _step = 0u; _step < MAX_SDF_STEPS; _step++)
        {
            float3 pos = probePos + (rayDir * t);
            float3 param_14 = pos;
            float d = sampleBestSDF(param_14);
            if (evidenceMode > 0u)
            {
                float3 param_15 = pos;
                _2643 = sampleHitEvidence(param_15);
            }
            else
            {
                _2643 = d;
            }
            float evidence = _2643;
            bool supportedHit = evidence < hitThreshold;
            if (d >= 1000000000.0f)
            {
                t += fallbackStride;
                if (t > volume_RayMaxDistance)
                {
                    hitDistance = -2.0f;
                    break;
                }
                continue;
            }
            if (((((signedCoarseField && clearedSkin) && supportedHit) && (d <= 0.0f)) && (prevD > 0.0f)) && (prevD < 1000000000.0f))
            {
                float tLo = prevT;
                float tHi = t;
                for (int b = 0; b < 5; b++)
                {
                    float tMid = 0.5f * (tLo + tHi);
                    if (evidenceMode > 0u)
                    {
                        float3 param_16 = probePos + (rayDir * tMid);
                        _2709 = sampleHitEvidence(param_16);
                    }
                    else
                    {
                        float3 param_17 = probePos + (rayDir * tMid);
                        _2709 = sampleBestSDF(param_17);
                    }
                    float dMid = _2709;
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
            if ((d < hitThreshold) && supportedHit)
            {
                if (!clearedSkin)
                {
                    t += max(d, minStep);
                    if (t > volume_RayMaxDistance)
                    {
                        hitDistance = -2.0f;
                        break;
                    }
                    continue;
                }
                hitDistance = t;
                break;
            }
            bool _2766 = evidenceMode == 0u;
            bool _2776;
            if (!_2766)
            {
                _2776 = (evidence >= hitThreshold) && (evidence < 1000000000.0f);
            }
            else
            {
                _2776 = _2766;
            }
            if (_2776)
            {
                clearedSkin = true;
            }
            if (((evidenceMode > 0u) && (d < hitThreshold)) && (!supportedHit))
            {
                if (evidenceMode >= 2u)
                {
                    _2793 = max(volume_SdfVoxelSizes[0].x * 2.0f, 0.20000000298023223876953125f);
                }
                else
                {
                    _2793 = max(d, minStep);
                }
                advance = _2793;
            }
            else
            {
                if ((d > 6.0f) && (d >= prevD))
                {
                    advance = d * 1.5f;
                }
                else
                {
                    advance = clamp(d, minStep, max(volume_SdfVoxelSizes[0].x * 2.0f, 0.20000000298023223876953125f));
                }
            }
            prevD = d;
            prevT = t;
            t += advance;
            if (t > volume_RayMaxDistance)
            {
                hitDistance = -2.0f;
                break;
            }
        }
        uint globalRayIdx = (group_id * volume_RaysPerProbe) + rayId;
        float3 hitN = 0.0f.xxx;
        float hnValid = 0.0f;
        if (hitDistance >= 0.0f)
        {
            float tRefined = hitDistance;
            float3 param_18 = probePos;
            float3 param_19 = rayDir;
            float param_20 = tRefined;
            bool _2862 = ddgiDetailRefine(param_18, param_19, param_20, param_21);
            tRefined = param_20;
            hitN = param_21;
            if (_2862)
            {
                hitDistance = tRefined;
                hnValid = 1.0f;
            }
            else
            {
                bool _2870 = evidenceMode > 0u;
                bool _2876;
                if (_2870)
                {
                    _2876 = volume_SdfVoxelSizes[0].z > 0.5f;
                }
                else
                {
                    _2876 = _2870;
                }
                bool _2890;
                if (_2876)
                {
                    float3 param_22 = probePos + (rayDir * hitDistance);
                    float3 param_23 = rayDir;
                    bool _2888 = evidenceHitNormal(param_22, param_23, param_24);
                    hitN = param_24;
                    _2890 = _2888;
                }
                else
                {
                    _2890 = _2876;
                }
                if (_2890)
                {
                    hnValid = 1.0f;
                }
            }
        }
        if (!depthOnly)
        {
            hn.Store4(globalRayIdx * 16 + 0, asuint(float4(hitN, hnValid)));
        }
        bool _2906 = !depthOnly;
        bool _2912;
        if (_2906)
        {
            _2912 = volume_SdfVoxelSizes[0].w > 0.5f;
        }
        else
        {
            _2912 = _2906;
        }
        if (_2912)
        {
            uint materialIdx = ((volume_ProbeCountTotal * volume_RaysPerProbe) + (4u * globalRayIdx)) + 3u;
            bool _2930 = (hitDistance >= 0.0f) && (evidenceMode > 0u);
            bool _2936;
            if (_2930)
            {
                _2936 = volume_SdfExtents[0].w > 0.5f;
            }
            else
            {
                _2936 = _2930;
            }
            bool _2951;
            if (_2936)
            {
                float3 param_25 = probePos + (rayDir * hitDistance);
                float param_26 = hitThreshold;
                bool _2949 = evidenceMaterialPoint(param_25, param_26, param_27);
                projected = param_27;
                _2951 = _2949;
            }
            else
            {
                _2951 = _2936;
            }
            bool valid = _2951;
            if (valid)
            {
                _2955 = float4(projected, 1.0f);
            }
            else
            {
                _2955 = 0.0f.xxxx;
            }
            hn.Store4(materialIdx * 16 + 0, asuint(_2955));
        }
        if (depthOnly)
        {
            globalRayIdx = (((volume_ProbeCountTotal * volume_RaysPerProbe) + (group_id * 128u)) + rayId) - volume_RaysPerProbe;
        }
        hd.Store(globalRayIdx * 4 + 0, asuint(hitDistance));
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
