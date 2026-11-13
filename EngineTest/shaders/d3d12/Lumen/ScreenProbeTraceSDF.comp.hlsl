struct SDFTraceResult
{
    bool hit;
    float _distance;
    float3 position;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer DetailSDFTable : register(b8, space0)
{
    float4 dtable_header : packoffset(c0);
    row_major float4x4 dtable_inv_world[4] : packoffset(c1);
    float4 dtable_normal_matrix[4][3] : packoffset(c17);
    float4 dtable_bounds_min[4] : packoffset(c29);
    float4 dtable_bounds_max[4] : packoffset(c33);
    float4 dtable_params[4] : packoffset(c37);
};

cbuffer GlobalData : register(b6, space0)
{
    row_major float4x4 global_view_projection : packoffset(c0);
    row_major float4x4 global_inv_view_projection : packoffset(c4);
    float4 global_camera_position : packoffset(c8);
    float4 global_grid_params : packoffset(c9);
    float4 global_trace_params : packoffset(c10);
    float4 global_sdf_origins[3] : packoffset(c11);
    float4 global_sdf_voxel_sizes[3] : packoffset(c14);
    float4 global_sdf_extents[3] : packoffset(c17);
    float4 global_sdf_resolutions : packoffset(c20);
    float4 global_surface_cache_params : packoffset(c21);
    float4 global_ddgi_fb[2] : packoffset(c22);
    float4 global_hzb_params : packoffset(c24);
    float4 global_cache_params : packoffset(c25);
    float4 global_diag_params : packoffset(c26);
    float4 global_hint_params : packoffset(c27);
    float4 global_sky_params : packoffset(c28);
    float4 global_wrc_fb[2] : packoffset(c29);
    float4 global_repr_params : packoffset(c31);
    float4 global_tier_params : packoffset(c32);
    float4 global_tier_counts : packoffset(c33);
};

ByteAddressBuffer compactList : register(t13, space0);
ByteAddressBuffer probeNormals : register(t4, space0);
RWByteAddressBuffer hit_buffer : register(u5, space0);
ByteAddressBuffer probePositions : register(t3, space0);
ByteAddressBuffer importanceSH : register(t7, space0);
Texture3D<float4> detail_sdf_0 : register(t9, space0);
Texture3D<float4> detail_sdf_1 : register(t10, space0);
Texture3D<float4> detail_sdf_2 : register(t11, space0);
Texture3D<float4> detail_sdf_3 : register(t12, space0);
Texture3D<float4> sdf0 : register(t0, space0);
Texture3D<float4> sdf1 : register(t1, space0);
Texture3D<float4> sdf2 : register(t2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint3 spvTextureSize(Texture3D<float4> Tex, uint Level, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, ret.z, Param);
    return ret;
}

uint spvPackSnorm2x16(float2 value)
{
    int2 Packed = int2(round(clamp(value, -1.0, 1.0) * 32767.0)) & 0xffff;
    return uint(Packed.x | (Packed.y << 16));
}

float2 spvUnpackSnorm2x16(uint value)
{
    int SignedValue = int(value);
    int2 Packed = int2(SignedValue << 16, SignedValue) >> 16;
    return clamp(float2(Packed) / 32767.0, -1.0, 1.0);
}

uint spgiJitterHash(inout uint v)
{
    v ^= (v >> 16u);
    v *= 2146121005u;
    v ^= (v >> 15u);
    v *= 2221713035u;
    v ^= (v >> 16u);
    return v;
}

float3x3 spgiJitterRotation(uint probeIdx, float frameSeed)
{
    if (frameSeed < 0.0f)
    {
        return float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f));
    }
    uint fs = uint(frameSeed);
    uint param = (probeIdx * 2654435769u) ^ fs;
    uint _632 = spgiJitterHash(param);
    uint h1 = _632;
    uint param_1 = h1 ^ 1757159915u;
    uint _638 = spgiJitterHash(param_1);
    uint h2 = _638;
    float a1 = float(h1) * 1.4629181199765639576071407645941e-09f;
    float a2 = float(h2) * 1.4629181199765639576071407645941e-09f;
    float c1 = cos(a1);
    float s1 = sin(a1);
    float c2 = cos(a2);
    float s2 = sin(a2);
    float3x3 rz1 = float3x3(float3(c1, s1, 0.0f), float3(-s1, c1, 0.0f), float3(0.0f, 0.0f, 1.0f));
    float3x3 rxf = float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, -1.0f), float3(0.0f, 1.0f, 0.0f));
    float3x3 rz2 = float3x3(float3(c2, s2, 0.0f), float3(-s2, c2, 0.0f), float3(0.0f, 0.0f, 1.0f));
    return mul(rz1, mul(rxf, rz2));
}

float3 fibonacciDirection(uint rayIndex, uint totalRays)
{
    float theta = (6.283185482025146484375f * float(rayIndex)) / 1.61803400516510009765625f;
    float phi = acos(1.0f - ((2.0f * (float(rayIndex) + 0.5f)) / float(totalRays)));
    return float3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

float3 spgiImportanceDirection(uint rayIdx, uint raysPerProbe, uint probeIdx, float frameSeed, float3 probeNormal, float3 sh0, float3 sh1, float3 sh2, float3 sh3, inout float invNq)
{
    uint n = min(raysPerProbe, 64u);
    uint param = probeIdx;
    float param_1 = frameSeed;
    float3x3 rot = spgiJitterRotation(param, param_1);
    bool3 _706 = (abs(probeNormal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_706.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _706.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _706.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, probeNormal));
    float3 bitangent = cross(probeNormal, tangent);
    float sum = 0.0f;
    float pdf[64];
    for (uint i = 0u; i < n; i++)
    {
        uint param_2 = i;
        uint param_3 = n;
        float3 local = mul(fibonacciDirection(param_2, param_3), rot);
        float3 world = ((tangent * local.x) + (bitangent * local.y)) + (probeNormal * local.z);
        float3 irr = (((sh0 * 0.2820949852466583251953125f) + (sh1 * ((-0.48860299587249755859375f) * world.y))) + (sh2 * (0.48860299587249755859375f * world.z))) + (sh3 * ((-0.48860299587249755859375f) * world.x));
        float lum = max(dot(irr, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)), 0.0f);
        pdf[i] = max(local.z, 0.0199999995529651641845703125f) * (lum + 0.0500000007450580596923828125f);
        sum += pdf[i];
    }
    for (uint i_1 = 0u; i_1 < n; i_1++)
    {
        pdf[i_1] = (0.25f / float(n)) + ((0.75f * pdf[i_1]) / max(sum, 9.9999999747524270787835121154785e-07f));
    }
    sum = 1.0f;
    uint _832;
    if (frameSeed < 0.0f)
    {
        _832 = 0u;
    }
    else
    {
        _832 = uint(frameSeed);
    }
    uint sampleSeed = _832;
    uint param_4 = ((probeIdx * 2654435769u) ^ sampleSeed) ^ (rayIdx * 2246822507u);
    uint _849 = spgiJitterHash(param_4);
    uint sampleHash = _849;
    float jitter = (float(sampleHash & 16777215u) + 0.5f) / 16777216.0f;
    float u = ((float(rayIdx) + jitter) / float(n)) * sum;
    uint c = n - 1u;
    float acc = 0.0f;
    for (uint i_2 = 0u; i_2 < n; i_2++)
    {
        acc += pdf[i_2];
        if (u <= acc)
        {
            c = i_2;
            break;
        }
    }
    invNq = sum / (float(n) * max(pdf[c], 9.9999999747524270787835121154785e-07f));
    uint param_5 = c;
    uint param_6 = n;
    return mul(fibonacciDirection(param_5, param_6), rot);
}

float3 orientAroundNormalTBN(float3 dir, float3 normal)
{
    bool3 _564 = (abs(normal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_564.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _564.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _564.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(((tangent * dir.x) + (bitangent * dir.y)) + (normal * dir.z));
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
    bool _147;
    if (_140)
    {
        _147 = local.y >= 0.0f;
    }
    else
    {
        _147 = _140;
    }
    bool _155;
    if (_147)
    {
        _155 = local.y < extent.y;
    }
    else
    {
        _155 = _147;
    }
    bool _162;
    if (_155)
    {
        _162 = local.z >= 0.0f;
    }
    else
    {
        _162 = _155;
    }
    bool _170;
    if (_162)
    {
        _170 = local.z < extent.z;
    }
    else
    {
        _170 = _162;
    }
    return _170;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _181 = uvw.x < 0.0f;
    bool _189;
    if (!_181)
    {
        _189 = uvw.x > 1.0f;
    }
    else
    {
        _189 = _181;
    }
    bool _196;
    if (!_189)
    {
        _196 = uvw.y < 0.0f;
    }
    else
    {
        _196 = _189;
    }
    bool _203;
    if (!_196)
    {
        _203 = uvw.y > 1.0f;
    }
    else
    {
        _203 = _196;
    }
    bool _210;
    if (!_203)
    {
        _210 = uvw.z < 0.0f;
    }
    else
    {
        _210 = _203;
    }
    bool _217;
    if (!_210)
    {
        _217 = uvw.z > 1.0f;
    }
    else
    {
        _217 = _210;
    }
    if (_217)
    {
        return 10000000000.0f;
    }
    uint _228_dummy_parameter;
    int3 size = int3(spvTextureSize(sdfTexture, uint(0), _228_dummy_parameter));
    float3 p = (clamp(uvw, 0.0f.xxx, 1.0f.xxx) * float3(size)) - 0.5f.xxx;
    int3 base = int3(floor(p));
    float3 f = p - float3(base);
    int3 c0 = clamp(base, int3(0, 0, 0), size - int3(1, 1, 1));
    int3 c1 = clamp(base + int3(1, 1, 1), int3(0, 0, 0), size - int3(1, 1, 1));
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

float sampleBestSDF(float3 pos, Texture3D<float4> sdf0_1, Texture3D<float4> sdf1_1, Texture3D<float4> sdf2_1, float4 cascadeOrigins[3], float4 cascadeExtents[3], uint cascadeCount)
{
    bool _384 = cascadeCount > 0u;
    bool _399;
    if (_384)
    {
        float3 param = pos;
        float3 param_1 = cascadeOrigins[0].xyz;
        float3 param_2 = cascadeExtents[0].xyz;
        _399 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _399 = _384;
    }
    if (_399)
    {
        float3 param_3 = pos;
        float3 param_4 = cascadeOrigins[0].xyz;
        float3 param_5 = cascadeExtents[0].xyz;
        return sampleSDFCascade(sdf0_1, param_3, param_4, param_5);
    }
    else
    {
        bool _416 = cascadeCount > 1u;
        bool _430;
        if (_416)
        {
            float3 param_6 = pos;
            float3 param_7 = cascadeOrigins[1].xyz;
            float3 param_8 = cascadeExtents[1].xyz;
            _430 = isInsideCascade(param_6, param_7, param_8);
        }
        else
        {
            _430 = _416;
        }
        if (_430)
        {
            float3 param_9 = pos;
            float3 param_10 = cascadeOrigins[1].xyz;
            float3 param_11 = cascadeExtents[1].xyz;
            return sampleSDFCascade(sdf1_1, param_9, param_10, param_11);
        }
        else
        {
            bool _447 = cascadeCount > 2u;
            bool _462;
            if (_447)
            {
                float3 param_12 = pos;
                float3 param_13 = cascadeOrigins[2].xyz;
                float3 param_14 = cascadeExtents[2].xyz;
                _462 = isInsideCascade(param_12, param_13, param_14);
            }
            else
            {
                _462 = _447;
            }
            if (_462)
            {
                float3 param_15 = pos;
                float3 param_16 = cascadeOrigins[2].xyz;
                float3 param_17 = cascadeExtents[2].xyz;
                return sampleSDFCascade(sdf2_1, param_15, param_16, param_17);
            }
        }
    }
    return 10000000000.0f;
}

SDFTraceResult traceSDFStepScale(float3 origin, float3 direction, float maxDist, float stepScale)
{
    SDFTraceResult result;
    result.hit = false;
    result.position = origin;
    result._distance = 0.0f;
    float3 pos = origin;
    float remaining = maxDist;
    float hitThreshold = global_sdf_voxel_sizes[0].x * 0.5f;
    float minStep = global_sdf_voxel_sizes[0].x * 0.25f;
    float4 param_1[3];
    float4 param_2[3];
    float _1830;
    for (uint _step = 0u; _step < 32u; _step++)
    {
        float3 param = pos;
        param_1[0] = global_sdf_origins[0];
        param_1[1] = global_sdf_origins[1];
        param_1[2] = global_sdf_origins[2];
        param_2[0] = global_sdf_extents[0];
        param_2[1] = global_sdf_extents[1];
        param_2[2] = global_sdf_extents[2];
        uint param_3 = uint(global_sdf_resolutions.w);
        float d = sampleBestSDF(param, sdf0, sdf1, sdf2, param_1, param_2, param_3);
        if (d < hitThreshold)
        {
            result.hit = true;
            result.position = pos;
            result._distance = length(pos - origin);
            return result;
        }
        if (d > (hitThreshold * 4.0f))
        {
            _1830 = d * stepScale;
        }
        else
        {
            _1830 = max(d, minStep);
        }
        float advance = _1830;
        if (advance > remaining)
        {
            break;
        }
        pos += (direction * advance);
        remaining -= advance;
    }
    result._distance = length(pos - origin);
    return result;
}

float detailSampleRaw(Texture3D<float4> tex, float3 bounds_min, float3 extent, float res, float3 local)
{
    float3 uvw = (local - bounds_min) / max(extent, 9.9999999747524270787835121154785e-07f.xxx);
    bool _923 = uvw.x < 0.0f;
    bool _930;
    if (!_923)
    {
        _930 = uvw.x > 1.0f;
    }
    else
    {
        _930 = _923;
    }
    bool _937;
    if (!_930)
    {
        _937 = uvw.y < 0.0f;
    }
    else
    {
        _937 = _930;
    }
    bool _944;
    if (!_937)
    {
        _944 = uvw.y > 1.0f;
    }
    else
    {
        _944 = _937;
    }
    bool _951;
    if (!_944)
    {
        _951 = uvw.z < 0.0f;
    }
    else
    {
        _951 = _944;
    }
    bool _958;
    if (!_951)
    {
        _958 = uvw.z > 1.0f;
    }
    else
    {
        _958 = _951;
    }
    if (_958)
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

bool detailSDFRefine(float3 rayOrigin, float3 rayDir, inout float t)
{
    uint count = uint(dtable_header.x);
    if (count == 0u)
    {
        return false;
    }
    float3 hitPos = rayOrigin + (rayDir * t);
    int best = -1;
    float bestAbs = 3.3999999521443642490773241379936e+38f;
    for (uint k = 0u; k < count; k++)
    {
        bool _1256 = any(bool3(hitPos.x < dtable_bounds_min[k].xyz.x, hitPos.y < dtable_bounds_min[k].xyz.y, hitPos.z < dtable_bounds_min[k].xyz.z));
        bool _1267;
        if (!_1256)
        {
            _1267 = any(bool3(hitPos.x > dtable_bounds_max[k].xyz.x, hitPos.y > dtable_bounds_max[k].xyz.y, hitPos.z > dtable_bounds_max[k].xyz.z));
        }
        else
        {
            _1267 = _1256;
        }
        if (_1267)
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
        bool haveLo = false;
        bool haveHi = false;
        for (int i = -5; i <= 5; i++)
        {
            float ti = t + (float(i) * localVoxel);
            uint param_4 = bk;
            float3 param_5 = mul(float4(rayOrigin + (rayDir * ti), 1.0f), dtable_inv_world[bk]).xyz;
            uint param_6 = bk;
            float param_7 = detailSampleK(param_4, param_5);
            float fi = detailDecode(param_6, param_7);
            bool _1379 = fi < 0.0f;
            bool _1388;
            if (_1379)
            {
                _1388 = (!haveLo) || (ti > tLo);
            }
            else
            {
                _1388 = _1379;
            }
            if (_1388)
            {
                tLo = ti;
                haveLo = true;
            }
            bool _1394 = fi >= 0.0f;
            bool _1403;
            if (_1394)
            {
                _1403 = (!haveHi) || (ti < tHi);
            }
            else
            {
                _1403 = _1394;
            }
            if (_1403)
            {
                tHi = ti;
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
                uint param_8 = bk;
                float3 param_9 = mul(float4(rayOrigin + (rayDir * tm), 1.0f), dtable_inv_world[bk]).xyz;
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
            uint param_12 = bk;
            float3 param_13 = mul(float4(rayOrigin + (rayDir * ti_1), 1.0f), dtable_inv_world[bk]).xyz;
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
    t = tBest;
    return true;
}

float sampleSDFTrilin(Texture3D<float4> tex, float3 pos, float3 origin, float3 extent, uint res)
{
    float3 uvw = (pos - origin) / extent;
    bool _1537 = uvw.x < 0.0f;
    bool _1544;
    if (!_1537)
    {
        _1544 = uvw.x > 1.0f;
    }
    else
    {
        _1544 = _1537;
    }
    bool _1551;
    if (!_1544)
    {
        _1551 = uvw.y < 0.0f;
    }
    else
    {
        _1551 = _1544;
    }
    bool _1558;
    if (!_1551)
    {
        _1558 = uvw.y > 1.0f;
    }
    else
    {
        _1558 = _1551;
    }
    bool _1565;
    if (!_1558)
    {
        _1565 = uvw.z < 0.0f;
    }
    else
    {
        _1565 = _1558;
    }
    bool _1572;
    if (!_1565)
    {
        _1572 = uvw.z > 1.0f;
    }
    else
    {
        _1572 = _1565;
    }
    if (_1572)
    {
        return 10000000000.0f;
    }
    float3 p = (clamp(uvw, 0.0f.xxx, 1.0f.xxx) * float(res).xxx) - 0.5f.xxx;
    int3 c0 = int3(floor(p));
    float3 f = frac(p);
    int3 c1 = min((c0 + int3(1, 1, 1)), (int(res) - 1).xxx);
    c0 = max(c0, int3(0, 0, 0));
    float v000 = tex.Load(int4(c0, 0)).x;
    float v100 = tex.Load(int4(int3(c1.x, c0.y, c0.z), 0)).x;
    float v010 = tex.Load(int4(int3(c0.x, c1.y, c0.z), 0)).x;
    float v110 = tex.Load(int4(int3(c1.x, c1.y, c0.z), 0)).x;
    float v001 = tex.Load(int4(int3(c0.x, c0.y, c1.z), 0)).x;
    float v101 = tex.Load(int4(int3(c1.x, c0.y, c1.z), 0)).x;
    float v011 = tex.Load(int4(int3(c0.x, c1.y, c1.z), 0)).x;
    float v111 = tex.Load(int4(int3(c1.x, c1.y, c1.z), 0)).x;
    return lerp(lerp(lerp(v000, v100, f.x), lerp(v010, v110, f.x), f.y), lerp(lerp(v001, v101, f.x), lerp(v011, v111, f.x), f.y), f.z);
}

float sampleBestSDFTrilinear(float3 pos)
{
    uint cc = uint(global_sdf_resolutions.w);
    if (cc > 0u)
    {
        float3 param = pos;
        float3 param_1 = global_sdf_origins[0].xyz;
        float3 param_2 = global_sdf_extents[0].xyz;
        uint param_3 = uint(global_sdf_resolutions.x);
        return sampleSDFTrilin(sdf0, param, param_1, param_2, param_3);
    }
    return 10000000000.0f;
}

float2 octEncode(float3 n)
{
    float l1norm = (abs(n.x) + abs(n.y)) + abs(n.z);
    float2 result = n.xy / l1norm.xx;
    if (n.z < 0.0f)
    {
        result = (1.0f.xx - abs(result.yx)) * lerp((-1.0f).xx, 1.0f.xx, step(0.0f.xx, result));
    }
    return (result * 0.5f) + 0.5f.xx;
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    uint gridW = uint(global_grid_params.x);
    uint gridH = uint(global_grid_params.y);
    uint raysPerProbe = uint(global_grid_params.w);
    uint probeIdx;
    uint rayIdx;
    if (global_tier_params.x > 0.5f)
    {
        probeIdx = compactList.Load((global_id / raysPerProbe) * 4 + 0);
        if (probeIdx == 4294967295u)
        {
            return;
        }
        rayIdx = global_id % raysPerProbe;
        bool _1913 = global_tier_params.w > 0.5f;
        bool _1926;
        if (_1913)
        {
            _1926 = float(rayIdx) >= asfloat(probeNormals.Load(probeIdx * 16 + 12));
        }
        else
        {
            _1926 = _1913;
        }
        if (_1926)
        {
            return;
        }
    }
    else
    {
        uint totalRays = (gridW * gridH) * raysPerProbe;
        if (global_id >= totalRays)
        {
            return;
        }
        probeIdx = global_id / raysPerProbe;
        rayIdx = global_id % raysPerProbe;
    }
    bool _1952 = global_hzb_params.z > 0.0f;
    bool _1963;
    if (_1952)
    {
        _1963 = asfloat(hit_buffer.Load(global_id * 16 + 12)) == 1.0f;
    }
    else
    {
        _1963 = _1952;
    }
    if (_1963)
    {
        return;
    }
    float4 posData = asfloat(probePositions.Load4(probeIdx * 16 + 0));
    if (posData.w <= 0.0f)
    {
        hit_buffer.Store4(global_id * 16 + 0, asuint(float4(-1.0f, 0.0f, 0.0f, 0.0f)));
        return;
    }
    float3 probeNormal = normalize(asfloat(probeNormals.Load4(probeIdx * 16 + 0)).xyz);
    float3 probePos = posData.xyz;
    float3 rayDir;
    if (global_cache_params.y > 0.5f)
    {
        uint hb = probeIdx * 4u;
        float4 h0 = asfloat(importanceSH.Load4((hb + 0u) * 16 + 0));
        float4 h1 = asfloat(importanceSH.Load4((hb + 1u) * 16 + 0));
        float4 h2 = asfloat(importanceSH.Load4((hb + 2u) * 16 + 0));
        float4 h3 = asfloat(importanceSH.Load4((hb + 3u) * 16 + 0));
        uint param = rayIdx;
        uint param_1 = raysPerProbe;
        uint param_2 = probeIdx;
        float param_3 = global_camera_position.w;
        float3 param_4 = probeNormal;
        float3 param_5 = h0.xyz;
        float3 param_6 = h1.xyz;
        float3 param_7 = h2.xyz;
        float3 param_8 = h3.xyz;
        float param_9;
        float3 _2052 = spgiImportanceDirection(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
        float invNq = param_9;
        rayDir = _2052;
        float3 param_10 = rayDir;
        float3 param_11 = probeNormal;
        rayDir = orientAroundNormalTBN(param_10, param_11);
    }
    else
    {
        uint param_12 = rayIdx;
        uint param_13 = raysPerProbe;
        rayDir = fibonacciDirection(param_12, param_13);
        uint param_14 = probeIdx;
        float param_15 = global_camera_position.w;
        rayDir = mul(rayDir, spgiJitterRotation(param_14, param_15));
        float3 param_16 = rayDir;
        float3 param_17 = probeNormal;
        rayDir = orientAroundNormalTBN(param_16, param_17);
    }
    float3 origin = probePos + ((probeNormal * global_sdf_voxel_sizes[0].x) * 0.25f);
    float cacheStart = global_cache_params.x;
    float _2092;
    if (cacheStart > 0.0f)
    {
        _2092 = min(global_trace_params.x, cacheStart);
    }
    else
    {
        _2092 = global_trace_params.x;
    }
    float marchDist = _2092;
    float3 param_18 = origin;
    float3 param_19 = rayDir;
    float param_20 = marchDist;
    float param_21 = global_hint_params.z;
    SDFTraceResult res = traceSDFStepScale(param_18, param_19, param_20, param_21);
    if (res.hit)
    {
        float t = res._distance;
        float3 param_22 = origin;
        float3 param_23 = rayDir;
        float param_24 = t;
        bool _2128 = detailSDFRefine(param_22, param_23, param_24);
        t = param_24;
        float3 hitPos = origin + (rayDir * t);
        float3 gNormal = 0.0f.xxx;
        float h = max(global_sdf_voxel_sizes[0].x, 9.9999997473787516355514526367188e-05f);
        float3 param_25 = hitPos + float3(h, 0.0f, 0.0f);
        float4 param_26[3];
        param_26[0] = global_sdf_origins[0];
        param_26[1] = global_sdf_origins[1];
        param_26[2] = global_sdf_origins[2];
        float4 param_27[3];
        param_27[0] = global_sdf_extents[0];
        param_27[1] = global_sdf_extents[1];
        param_27[2] = global_sdf_extents[2];
        uint param_28 = uint(global_sdf_resolutions.w);
        float3 param_29 = hitPos - float3(h, 0.0f, 0.0f);
        float4 param_30[3];
        param_30[0] = global_sdf_origins[0];
        param_30[1] = global_sdf_origins[1];
        param_30[2] = global_sdf_origins[2];
        float4 param_31[3];
        param_31[0] = global_sdf_extents[0];
        param_31[1] = global_sdf_extents[1];
        param_31[2] = global_sdf_extents[2];
        uint param_32 = uint(global_sdf_resolutions.w);
        float dx = sampleBestSDF(param_25, sdf0, sdf1, sdf2, param_26, param_27, param_28) - sampleBestSDF(param_29, sdf0, sdf1, sdf2, param_30, param_31, param_32);
        float3 param_33 = hitPos + float3(0.0f, h, 0.0f);
        float4 param_34[3];
        param_34[0] = global_sdf_origins[0];
        param_34[1] = global_sdf_origins[1];
        param_34[2] = global_sdf_origins[2];
        float4 param_35[3];
        param_35[0] = global_sdf_extents[0];
        param_35[1] = global_sdf_extents[1];
        param_35[2] = global_sdf_extents[2];
        uint param_36 = uint(global_sdf_resolutions.w);
        float3 param_37 = hitPos - float3(0.0f, h, 0.0f);
        float4 param_38[3];
        param_38[0] = global_sdf_origins[0];
        param_38[1] = global_sdf_origins[1];
        param_38[2] = global_sdf_origins[2];
        float4 param_39[3];
        param_39[0] = global_sdf_extents[0];
        param_39[1] = global_sdf_extents[1];
        param_39[2] = global_sdf_extents[2];
        uint param_40 = uint(global_sdf_resolutions.w);
        float dy = sampleBestSDF(param_33, sdf0, sdf1, sdf2, param_34, param_35, param_36) - sampleBestSDF(param_37, sdf0, sdf1, sdf2, param_38, param_39, param_40);
        float3 param_41 = hitPos + float3(0.0f, 0.0f, h);
        float4 param_42[3];
        param_42[0] = global_sdf_origins[0];
        param_42[1] = global_sdf_origins[1];
        param_42[2] = global_sdf_origins[2];
        float4 param_43[3];
        param_43[0] = global_sdf_extents[0];
        param_43[1] = global_sdf_extents[1];
        param_43[2] = global_sdf_extents[2];
        uint param_44 = uint(global_sdf_resolutions.w);
        float3 param_45 = hitPos - float3(0.0f, 0.0f, h);
        float4 param_46[3];
        param_46[0] = global_sdf_origins[0];
        param_46[1] = global_sdf_origins[1];
        param_46[2] = global_sdf_origins[2];
        float4 param_47[3];
        param_47[0] = global_sdf_extents[0];
        param_47[1] = global_sdf_extents[1];
        param_47[2] = global_sdf_extents[2];
        uint param_48 = uint(global_sdf_resolutions.w);
        float dz = sampleBestSDF(param_41, sdf0, sdf1, sdf2, param_42, param_43, param_44) - sampleBestSDF(param_45, sdf0, sdf1, sdf2, param_46, param_47, param_48);
        gNormal = normalize(float3(dx, dy, dz) + 9.9999999747524270787835121154785e-07f.xxx);
        if (global_hint_params.x > 0.5f)
        {
            float3 param_49 = hitPos + float3(h, 0.0f, 0.0f);
            float3 param_50 = hitPos - float3(h, 0.0f, 0.0f);
            float tdx = sampleBestSDFTrilinear(param_49) - sampleBestSDFTrilinear(param_50);
            float3 param_51 = hitPos + float3(0.0f, h, 0.0f);
            float3 param_52 = hitPos - float3(0.0f, h, 0.0f);
            float tdy = sampleBestSDFTrilinear(param_51) - sampleBestSDFTrilinear(param_52);
            float3 param_53 = hitPos + float3(0.0f, 0.0f, h);
            float3 param_54 = hitPos - float3(0.0f, 0.0f, h);
            float tdz = sampleBestSDFTrilinear(param_53) - sampleBestSDFTrilinear(param_54);
            float3 tn = normalize(float3(tdx, tdy, tdz) + 9.9999999747524270787835121154785e-07f.xxx);
            gNormal = normalize(lerp(gNormal, tn, 0.75f.xxx));
        }
        float3 param_55 = gNormal;
        float2 oct = octEncode(param_55);
        float2 octS = clamp((oct * 2.0f) - 1.0f.xx, (-1.0f).xx, 1.0f.xx);
        bool _2400 = global_diag_params.x > 5.5f;
        bool _2407;
        if (_2400)
        {
            _2407 = global_diag_params.x < 6.5f;
        }
        else
        {
            _2407 = _2400;
        }
        float _2408;
        if (_2407)
        {
            _2408 = res._distance;
        }
        else
        {
            _2408 = 0.0f;
        }
        hit_buffer.Store4(global_id * 16 + 0, asuint(float4(t, asfloat(spvPackSnorm2x16(octS)), _2408, 2.0f)));
    }
    else
    {
        if (cacheStart > 0.0f)
        {
            hit_buffer.Store4(global_id * 16 + 0, asuint(float4(res._distance, 0.0f, 0.0f, 3.0f)));
        }
        else
        {
            hit_buffer.Store4(global_id * 16 + 0, asuint(float4(-1.0f, 0.0f, 0.0f, 0.0f)));
        }
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
