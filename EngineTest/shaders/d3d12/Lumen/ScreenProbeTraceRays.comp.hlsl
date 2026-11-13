struct SurfaceCacheCard
{
    float4 center;
    float4 extent;
    uint axis_direction;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint mesh_instance_id;
    uint static_mesh_slot;
    uint flags;
    uint page_card;
};

struct SCGridParams
{
    float3 grid_min;
    float3 grid_max;
    uint3 dims;
    float cell_size;
    uint card_count;
};

struct SDFTraceResult
{
    bool hit;
    float _distance;
    float3 position;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

struct SurfaceCacheCardLookup
{
    float4 aabb_min;
    float4 aabb_max;
    uint card_start;
    uint card_count;
    uint _pad0;
    uint _pad1;
};

cbuffer GlobalBlock : register(b8, space0)
{
    row_major float4x4 global_view_projection : packoffset(c0);
    row_major float4x4 global_inv_view_projection : packoffset(c4);
    float4 global_camera_position : packoffset(c8);
    float4 global_grid_params : packoffset(c9);
    float4 global_trace_params : packoffset(c10);
    float4 global_sdf_origins[3] : packoffset(c11);
    float4 global_sdf_voxel_sizes[3] : packoffset(c14);
    float4 global_sdf_extents[3] : packoffset(c17);
    uint4 global_sdf_resolutions : packoffset(c20);
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

ByteAddressBuffer compactList : register(t15, space0);
ByteAddressBuffer probeNormals : register(t6, space0);
RWByteAddressBuffer probeRadiance : register(u7, space0);
ByteAddressBuffer probePositions : register(t5, space0);
ByteAddressBuffer importanceSH : register(t14, space0);
ByteAddressBuffer scgo : register(t11, space0);
ByteAddressBuffer cards : register(t9, space0);
ByteAddressBuffer scgc : register(t12, space0);
ByteAddressBuffer ddgiIrr : register(t13, space0);
ByteAddressBuffer cardLookup : register(t10, space0);
Texture2D<float4> prevFrameColor : register(t3, space0);
Texture3D<float4> sdf0 : register(t0, space0);
Texture3D<float4> sdf1 : register(t1, space0);
Texture3D<float4> sdf2 : register(t2, space0);
Texture2D<float4> surface_cache_lighting : register(t4, space0);

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

uint3 spvTextureSize(Texture3D<float4> Tex, uint Level, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, ret.z, Param);
    return ret;
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
    uint _1057 = spgiJitterHash(param);
    uint h1 = _1057;
    uint param_1 = h1 ^ 1757159915u;
    uint _1063 = spgiJitterHash(param_1);
    uint h2 = _1063;
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
    bool3 _1131 = (abs(probeNormal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_1131.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _1131.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _1131.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
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
    uint _1256;
    if (frameSeed < 0.0f)
    {
        _1256 = 0u;
    }
    else
    {
        _1256 = uint(frameSeed);
    }
    uint sampleSeed = _1256;
    uint param_4 = ((probeIdx * 2654435769u) ^ sampleSeed) ^ (rayIdx * 2246822507u);
    uint _1273 = spgiJitterHash(param_4);
    uint sampleHash = _1273;
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
    bool3 _989 = (abs(normal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_989.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _989.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _989.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(((tangent * dir.x) + (bitangent * dir.y)) + (normal * dir.z));
}

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _517 = local.x >= 0.0f;
    bool _525;
    if (_517)
    {
        _525 = local.x < extent.x;
    }
    else
    {
        _525 = _517;
    }
    bool _531;
    if (_525)
    {
        _531 = local.y >= 0.0f;
    }
    else
    {
        _531 = _525;
    }
    bool _539;
    if (_531)
    {
        _539 = local.y < extent.y;
    }
    else
    {
        _539 = _531;
    }
    bool _545;
    if (_539)
    {
        _545 = local.z >= 0.0f;
    }
    else
    {
        _545 = _539;
    }
    bool _553;
    if (_545)
    {
        _553 = local.z < extent.z;
    }
    else
    {
        _553 = _545;
    }
    return _553;
}

float sampleSDFCascade(Texture3D<float4> sdfTexture, float3 worldPos, float3 cascadeOrigin, float3 cascadeExtent)
{
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    bool _564 = uvw.x < 0.0f;
    bool _571;
    if (!_564)
    {
        _571 = uvw.x > 1.0f;
    }
    else
    {
        _571 = _564;
    }
    bool _578;
    if (!_571)
    {
        _578 = uvw.y < 0.0f;
    }
    else
    {
        _578 = _571;
    }
    bool _585;
    if (!_578)
    {
        _585 = uvw.y > 1.0f;
    }
    else
    {
        _585 = _578;
    }
    bool _592;
    if (!_585)
    {
        _592 = uvw.z < 0.0f;
    }
    else
    {
        _592 = _585;
    }
    bool _599;
    if (!_592)
    {
        _599 = uvw.z > 1.0f;
    }
    else
    {
        _599 = _592;
    }
    if (_599)
    {
        return 10000000000.0f;
    }
    uint _608_dummy_parameter;
    int3 size = int3(spvTextureSize(sdfTexture, uint(0), _608_dummy_parameter));
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
    bool _762 = cascadeCount > 0u;
    bool _776;
    if (_762)
    {
        float3 param = pos;
        float3 param_1 = cascadeOrigins[0].xyz;
        float3 param_2 = cascadeExtents[0].xyz;
        _776 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _776 = _762;
    }
    if (_776)
    {
        float3 param_3 = pos;
        float3 param_4 = cascadeOrigins[0].xyz;
        float3 param_5 = cascadeExtents[0].xyz;
        return sampleSDFCascade(sdf0_1, param_3, param_4, param_5);
    }
    else
    {
        bool _793 = cascadeCount > 1u;
        bool _807;
        if (_793)
        {
            float3 param_6 = pos;
            float3 param_7 = cascadeOrigins[1].xyz;
            float3 param_8 = cascadeExtents[1].xyz;
            _807 = isInsideCascade(param_6, param_7, param_8);
        }
        else
        {
            _807 = _793;
        }
        if (_807)
        {
            float3 param_9 = pos;
            float3 param_10 = cascadeOrigins[1].xyz;
            float3 param_11 = cascadeExtents[1].xyz;
            return sampleSDFCascade(sdf1_1, param_9, param_10, param_11);
        }
        else
        {
            bool _824 = cascadeCount > 2u;
            bool _838;
            if (_824)
            {
                float3 param_12 = pos;
                float3 param_13 = cascadeOrigins[2].xyz;
                float3 param_14 = cascadeExtents[2].xyz;
                _838 = isInsideCascade(param_12, param_13, param_14);
            }
            else
            {
                _838 = _824;
            }
            if (_838)
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

SDFTraceResult traceSDF_relaxed(float3 origin, float3 direction, float maxDist, Texture3D<float4> sdf0_1, Texture3D<float4> sdf1_1, Texture3D<float4> sdf2_1, float4 cascadeOrigins[3], float4 cascadeExtents[3], float4 voxelSizes[3], uint cascadeCount)
{
    SDFTraceResult result;
    result.hit = false;
    result.position = origin;
    result._distance = 0.0f;
    float3 pos = origin;
    float remainingDist = maxDist;
    float hitThreshold = voxelSizes[0].x * 0.5f;
    float minStep = voxelSizes[0].x * 0.25f;
    float advance;
    for (uint _step = 0u; _step < 32u; _step++)
    {
        float3 param = pos;
        float4 param_1[3] = cascadeOrigins;
        float4 param_2[3] = cascadeExtents;
        uint param_3 = cascadeCount;
        float d = sampleBestSDF(param, sdf0_1, sdf1_1, sdf2_1, param_1, param_2, param_3);
        if (d < hitThreshold)
        {
            result.hit = true;
            result.position = pos;
            result._distance = length(pos - origin);
            return result;
        }
        if (d > (hitThreshold * 4.0f))
        {
            advance = d * 1.2000000476837158203125f;
        }
        else
        {
            advance = max(d, minStep);
        }
        if (advance > remainingDist)
        {
            break;
        }
        pos += (direction * advance);
        remainingDist -= advance;
    }
    result._distance = length(pos - origin);
    return result;
}

SCGridParams scGridDecode(uint4 h0, uint4 h1, uint4 h2)
{
    SCGridParams gp;
    gp.grid_min = asfloat(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = asfloat(h1.xyz);
    gp.cell_size = asfloat(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

uint scGridCellIndex(SCGridParams gp, float3 p)
{
    float3 g = (p - gp.grid_min) / gp.cell_size.xxx;
    bool _447 = g.x < 0.0f;
    bool _454;
    if (!_447)
    {
        _454 = g.y < 0.0f;
    }
    else
    {
        _454 = _447;
    }
    bool _461;
    if (!_454)
    {
        _461 = g.z < 0.0f;
    }
    else
    {
        _461 = _454;
    }
    if (_461)
    {
        return 4294967295u;
    }
    uint3 c = uint3(g);
    bool _473 = c.x >= gp.dims.x;
    bool _482;
    if (!_473)
    {
        _482 = c.y >= gp.dims.y;
    }
    else
    {
        _482 = _473;
    }
    bool _491;
    if (!_482)
    {
        _491 = c.z >= gp.dims.z;
    }
    else
    {
        _491 = _482;
    }
    if (_491)
    {
        return 4294967295u;
    }
    return c.x + (gp.dims.x * (c.y + (gp.dims.y * c.z)));
}

bool worldToCardUVMarginPad(float3 world_pos, SurfaceCacheCard card, inout float2 out_uv, float margin_uv, float pad_m)
{
    float3 local = world_pos - card.center.xyz;
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float depth;
    float u;
    float v;
    if (axis == 0u)
    {
        depth = local.x * ((dir > 0u) ? 1.0f : (-1.0f));
        u = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
    }
    else
    {
        if (axis == 1u)
        {
            depth = local.y * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
        }
        else
        {
            depth = local.z * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        }
    }
    if (depth < 0.0f)
    {
        return false;
    }
    float padU = 0.0f;
    float padV = 0.0f;
    if (pad_m > 0.0f)
    {
        float _259;
        if (axis == 0u)
        {
            _259 = card.extent.y;
        }
        else
        {
            float _267;
            if (axis == 1u)
            {
                _267 = card.extent.x;
            }
            else
            {
                _267 = card.extent.x;
            }
            _259 = _267;
        }
        float eu = _259;
        float _280;
        if (axis == 0u)
        {
            _280 = card.extent.z;
        }
        else
        {
            float _288;
            if (axis == 1u)
            {
                _288 = card.extent.z;
            }
            else
            {
                _288 = card.extent.y;
            }
            _280 = _288;
        }
        float ev = _280;
        float _301;
        if (eu > 9.9999997473787516355514526367188e-05f)
        {
            _301 = pad_m / (eu * 2.0f);
        }
        else
        {
            _301 = 0.0f;
        }
        padU = _301;
        float _312;
        if (ev > 9.9999997473787516355514526367188e-05f)
        {
            _312 = pad_m / (ev * 2.0f);
        }
        else
        {
            _312 = 0.0f;
        }
        padV = _312;
    }
    bool _326 = u < ((-margin_uv) - padU);
    bool _336;
    if (!_326)
    {
        _336 = u >= ((1.0f + margin_uv) + padU);
    }
    else
    {
        _336 = _326;
    }
    bool _346;
    if (!_336)
    {
        _346 = v < ((-margin_uv) - padV);
    }
    else
    {
        _346 = _336;
    }
    bool _356;
    if (!_346)
    {
        _356 = v >= ((1.0f + margin_uv) + padV);
    }
    else
    {
        _356 = _346;
    }
    if (_356)
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999998986721038818359375f);
    v = clamp(v, 0.0f, 0.999998986721038818359375f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

bool worldToCardUVMargin(float3 world_pos, SurfaceCacheCard card, out float2 out_uv, float margin_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = margin_uv;
    float param_4 = 0.0f;
    float2 param_2;
    bool _399 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    out_uv = param_2;
    return _399;
}

bool worldToCardUV(float3 world_pos, SurfaceCacheCard card, out float2 out_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 0.0f;
    float2 param_2;
    bool _409 = worldToCardUVMargin(param, param_1, param_2, param_3);
    out_uv = param_2;
    return _409;
}

float3 sampleBilinearRGB(Texture2D<float4> tex, float2 uv)
{
    uint _1340_dummy_parameter;
    float2 size = float2(int2(spvTextureSize(tex, uint(0), _1340_dummy_parameter)));
    float2 px = (uv * size) - 0.5f.xx;
    int2 p0 = int2(floor(px));
    float2 f = frac(px);
    int2 c00 = clamp(p0, int2(0, 0), int2(size) - int2(1, 1));
    int2 c10 = clamp(p0 + int2(1, 0), int2(0, 0), int2(size) - int2(1, 1));
    int2 c01 = clamp(p0 + int2(0, 1), int2(0, 0), int2(size) - int2(1, 1));
    int2 c11 = clamp(p0 + int2(1, 1), int2(0, 0), int2(size) - int2(1, 1));
    float3 s00 = tex.Load(int3(c00, 0)).xyz;
    float3 s10 = tex.Load(int3(c10, 0)).xyz;
    float3 s01 = tex.Load(int3(c01, 0)).xyz;
    float3 s11 = tex.Load(int3(c11, 0)).xyz;
    float3 a = lerp(s00, s10, f.x.xxx);
    float3 b = lerp(s01, s11, f.x.xxx);
    return lerp(a, b, f.y.xxx);
}

float3 samplePrevFrameColor(float3 worldPos)
{
    float4 clipPos = mul(float4(worldPos, 1.0f), global_view_projection);
    if (clipPos.w <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float2 ndc = clipPos.xy / clipPos.w.xx;
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, (1.0f - (ndc.y * 0.5f)) - 0.5f);
    bool _1478 = uv.x < 0.0f;
    bool _1485;
    if (!_1478)
    {
        _1485 = uv.x > 1.0f;
    }
    else
    {
        _1485 = _1478;
    }
    bool _1492;
    if (!_1485)
    {
        _1492 = uv.y < 0.0f;
    }
    else
    {
        _1492 = _1485;
    }
    bool _1499;
    if (!_1492)
    {
        _1499 = uv.y > 1.0f;
    }
    else
    {
        _1499 = _1492;
    }
    if (_1499)
    {
        return 0.0f.xxx;
    }
    float2 param = uv;
    return sampleBilinearRGB(prevFrameColor, param);
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
        bool _1556 = global_tier_params.w > 0.5f;
        bool _1569;
        if (_1556)
        {
            _1569 = float(rayIdx) >= asfloat(probeNormals.Load(probeIdx * 16 + 12));
        }
        else
        {
            _1569 = _1556;
        }
        if (_1569)
        {
            probeRadiance.Store4(global_id * 16 + 0, asuint(0.0f.xxxx));
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
    float4 posData = asfloat(probePositions.Load4(probeIdx * 16 + 0));
    float4 normData = asfloat(probeNormals.Load4(probeIdx * 16 + 0));
    if (posData.w <= 0.0f)
    {
        probeRadiance.Store4(global_id * 16 + 0, asuint(float4(0.0f, 0.0f, 0.0f, -1.0f)));
        return;
    }
    float3 probePos = posData.xyz;
    float3 probeNormal = normalize(normData.xyz);
    float invNq = 1.0f;
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
        float3 _1688 = spgiImportanceDirection(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
        invNq = param_9;
        rayDir = _1688;
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
    float3 origin = probePos + ((probeNormal * global_sdf_voxel_sizes[0].x) * 2.0f);
    float cacheStart = global_cache_params.x;
    float _1729;
    if (cacheStart > 0.0f)
    {
        _1729 = min(global_trace_params.x, cacheStart);
    }
    else
    {
        _1729 = global_trace_params.x;
    }
    float maxDist = _1729;
    float3 param_18 = origin;
    float3 param_19 = rayDir;
    float param_20 = maxDist;
    float4 param_21[3];
    param_21[0] = global_sdf_origins[0];
    param_21[1] = global_sdf_origins[1];
    param_21[2] = global_sdf_origins[2];
    float4 param_22[3];
    param_22[0] = global_sdf_extents[0];
    param_22[1] = global_sdf_extents[1];
    param_22[2] = global_sdf_extents[2];
    float4 param_23[3];
    param_23[0] = global_sdf_voxel_sizes[0];
    param_23[1] = global_sdf_voxel_sizes[1];
    param_23[2] = global_sdf_voxel_sizes[2];
    uint param_24 = global_sdf_resolutions.w;
    SDFTraceResult hit = traceSDF_relaxed(param_18, param_19, param_20, sdf0, sdf1, sdf2, param_21, param_22, param_23, param_24);
    float3 radiance = 0.0f.xxx;
    float cosTheta = 0.0f;
    if (hit.hit)
    {
        bool usedSurfaceCache = false;
        float nearDistance = 2.0f;
        bool _1796 = hit._distance < nearDistance;
        bool _1803;
        if (_1796)
        {
            _1803 = global_surface_cache_params.z > 0.5f;
        }
        else
        {
            _1803 = _1796;
        }
        if (_1803)
        {
            float atlasSize = global_surface_cache_params.x;
            uint4 gh0 = uint4(scgo.Load(0), scgo.Load(4), scgo.Load(8), scgo.Load(12));
            uint4 gh1 = uint4(scgo.Load(16), scgo.Load(20), scgo.Load(24), scgo.Load(28));
            uint4 gh2 = uint4(scgo.Load(32), scgo.Load(36), scgo.Load(40), scgo.Load(44));
            uint4 param_25 = gh0;
            uint4 param_26 = gh1;
            uint4 param_27 = gh2;
            SCGridParams gp = scGridDecode(param_25, param_26, param_27);
            SCGridParams param_28 = gp;
            float3 param_29 = hit.position;
            uint cell = scGridCellIndex(param_28, param_29);
            if (cell != 4294967295u)
            {
                uint begin = scgo.Load((16u + cell) * 4 + 0);
                uint end = scgo.Load((17u + cell) * 4 + 0);
                SurfaceCacheCard param_31;
                float2 param_32;
                for (uint gi = begin; (gi < end) && (!usedSurfaceCache); gi++)
                {
                    float3 param_30 = hit.position;
                    SurfaceCacheCard _1907;
                    _1907.center = asfloat(cards.Load4(scgc.Load(gi * 4 + 0) * 64 + 0));
                    _1907.extent = asfloat(cards.Load4(scgc.Load(gi * 4 + 0) * 64 + 16));
                    _1907.axis_direction = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 32);
                    _1907.resolution = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 36);
                    _1907.atlas_offset_x = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 40);
                    _1907.atlas_offset_y = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 44);
                    _1907.mesh_instance_id = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 48);
                    _1907.static_mesh_slot = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 52);
                    _1907.flags = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 56);
                    _1907.page_card = cards.Load(scgc.Load(gi * 4 + 0) * 64 + 60);
                    param_31.center = _1907.center;
                    param_31.extent = _1907.extent;
                    param_31.axis_direction = _1907.axis_direction;
                    param_31.resolution = _1907.resolution;
                    param_31.atlas_offset_x = _1907.atlas_offset_x;
                    param_31.atlas_offset_y = _1907.atlas_offset_y;
                    param_31.mesh_instance_id = _1907.mesh_instance_id;
                    param_31.static_mesh_slot = _1907.static_mesh_slot;
                    param_31.flags = _1907.flags;
                    param_31.page_card = _1907.page_card;
                    bool _1929 = worldToCardUV(param_30, param_31, param_32);
                    float2 atlas_uv = param_32;
                    if (_1929)
                    {
                        float2 uv_norm = atlas_uv / atlasSize.xx;
                        float2 param_33 = uv_norm;
                        radiance = sampleBilinearRGB(surface_cache_lighting, param_33);
                        usedSurfaceCache = true;
                    }
                }
            }
        }
        if (!usedSurfaceCache)
        {
            float3 param_34 = hit.position;
            radiance = samplePrevFrameColor(param_34);
        }
        bool _1954 = any(isnan(radiance));
        bool _1961;
        if (!_1954)
        {
            _1961 = any(isinf(radiance));
        }
        else
        {
            _1961 = _1954;
        }
        if (_1961)
        {
            radiance = 0.0f.xxx;
        }
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    }
    else
    {
        float3 _1972;
        if (cacheStart > 0.0f)
        {
            _1972 = origin + (rayDir * hit._distance);
        }
        else
        {
            _1972 = probePos;
        }
        float3 queryPos = _1972;
        if (global_ddgi_fb[1].w > 0.5f)
        {
            float3 org = global_ddgi_fb[0].xyz;
            float spacing = max(global_ddgi_fb[0].w, 0.00999999977648258209228515625f);
            int3 counts = int3(global_ddgi_fb[1].xyz);
            int3 gc = int3(floor((queryPos - org) / spacing.xxx));
            gc = clamp(gc, int3(0, 0, 0), counts - int3(1, 1, 1));
            uint pidx = uint(gc.x) + (uint(counts.x) * (uint(gc.y) + (uint(counts.y) * uint(gc.z))));
            float3 sh0 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 0u) * 16 + 0)).xyz;
            float3 sh1 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 1u) * 16 + 0)).xyz;
            float3 sh2 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 2u) * 16 + 0)).xyz;
            float3 sh3 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 3u) * 16 + 0)).xyz;
            radiance = max(0.0f.xxx, (((sh0 * 0.2820949852466583251953125f) - ((sh1 * 0.48860299587249755859375f) * rayDir.y)) + ((sh2 * 0.48860299587249755859375f) * rayDir.z)) - ((sh3 * 0.48860299587249755859375f) * rayDir.x));
            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
        }
        else
        {
            float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight > 0.0f)
            {
                float skyY = (rayDir.y * 0.5f) + 0.5f;
                radiance = lerp(float3(0.1500000059604644775390625f, 0.180000007152557373046875f, 0.25f), float3(0.4000000059604644775390625f, 0.5f, 0.64999997615814208984375f), skyY.xxx);
                cosTheta = skyWeight;
            }
        }
    }
    float _2122;
    if (hit.hit)
    {
        _2122 = cosTheta;
    }
    else
    {
        _2122 = -cosTheta;
    }
    float outputWeight = _2122 * invNq;
    probeRadiance.Store4(global_id * 16 + 0, asuint(float4(radiance, outputWeight)));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
