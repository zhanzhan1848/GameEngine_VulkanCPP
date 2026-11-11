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

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

static const float4 _5988[6] = { float4(0.4600000083446502685546875f, 0.2599999904632568359375f, 0.60000002384185791015625f, 0.439999997615814208984375f), float4(0.0f, 0.4199999868869781494140625f, 0.2199999988079071044921875f, 0.7799999713897705078125f), float4(0.800000011920928955078125f, 0.4199999868869781494140625f, 1.0f, 0.7799999713897705078125f), float4(0.4199999868869781494140625f, 0.5f, 0.579999983310699462890625f, 0.75f), float4(0.439999997615814208984375f, 0.100000001490116119384765625f, 0.550000011920928955078125f, 0.2199999988079071044921875f), float4(0.300000011920928955078125f, 0.87999999523162841796875f, 0.699999988079071044921875f, 1.0f) };

ByteAddressBuffer geometryTop : register(t26, space0);
ByteAddressBuffer geometryInstances : register(t29, space0);
ByteAddressBuffer geometryBottom : register(t27, space0);
ByteAddressBuffer geometryTriangles : register(t28, space0);
cbuffer GlobalData : register(b5, space0)
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

ByteAddressBuffer _2317 : register(t19, space0);
RWByteAddressBuffer _2463 : register(u20, space0);
RWByteAddressBuffer _2657 : register(u18, space0);
ByteAddressBuffer compactList : register(t21, space0);
RWByteAddressBuffer probeRadiance : register(u6, space0);
ByteAddressBuffer probeNormals : register(t3, space0);
ByteAddressBuffer probePositions : register(t2, space0);
ByteAddressBuffer importanceSH : register(t12, space0);
ByteAddressBuffer hit_buffer : register(t4, space0);
ByteAddressBuffer zeroOwners : register(t30, space0);
ByteAddressBuffer zeroMaterials : register(t31, space0);
ByteAddressBuffer cardLookup : register(t8, space0);
ByteAddressBuffer cards : register(t7, space0);
ByteAddressBuffer scgo : register(t9, space0);
ByteAddressBuffer scgc : register(t10, space0);
RWByteAddressBuffer _3600 : register(u22, space0);
ByteAddressBuffer ddgiIrr : register(t11, space0);
ByteAddressBuffer ddgiOrigins : register(t25, space0);
ByteAddressBuffer ddgiDepth : register(t24, space0);
Texture2D<float4> prevFrameColor : register(t0, space0);
Texture2D<float4> surface_cache_depth : register(t17, space0);
Texture2D<float4> surface_cache_lighting : register(t1, space0);
Texture2D<float4> surface_cache_albedo : register(t23, space0);
Texture3D<float4> radianceCascadeL0 : register(t13, space0);
Texture3D<float4> radianceCascadeL1x : register(t14, space0);
Texture3D<float4> radianceCascadeL1y : register(t15, space0);
Texture3D<float4> radianceCascadeL1z : register(t16, space0);

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

uint spvPackHalf2x16(float2 value)
{
    uint2 Packed = f32tof16(value);
    return Packed.x | (Packed.y << 16);
}

float2 spvUnpackHalf2x16(uint value)
{
    return f16tof32(uint2(value & 0xffff, value >> 16));
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
    uint _1029 = spgiJitterHash(param);
    uint h1 = _1029;
    uint param_1 = h1 ^ 1757159915u;
    uint _1035 = spgiJitterHash(param_1);
    uint h2 = _1035;
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
    bool3 _1103 = (abs(probeNormal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_1103.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _1103.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _1103.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
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
    uint _1227;
    if (frameSeed < 0.0f)
    {
        _1227 = 0u;
    }
    else
    {
        _1227 = uint(frameSeed);
    }
    uint sampleSeed = _1227;
    uint param_4 = ((probeIdx * 2654435769u) ^ sampleSeed) ^ (rayIdx * 2246822507u);
    uint _1244 = spgiJitterHash(param_4);
    uint sampleHash = _1244;
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
    bool3 _961 = (abs(normal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_961.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _961.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _961.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(((tangent * dir.x) + (bitangent * dir.y)) + (normal * dir.z));
}

float3 sampleBilinearRGB(Texture2D<float4> tex, float2 uv)
{
    uint _2474_dummy_parameter;
    float2 size = float2(int2(spvTextureSize(tex, uint(0), _2474_dummy_parameter)));
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

float3 samplePrevFrameColorAt(float2 uv)
{
    float2 param = uv;
    return sampleBilinearRGB(prevFrameColor, param);
}

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _808 = n;
    float3 _812 = n;
    float2 _814 = _812.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _808.xy));
    n.x = _814.x;
    n.y = _814.y;
    return normalize(n);
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
            bool _1414 = o[k] < n.lo[k];
            bool _1425;
            if (!_1414)
            {
                _1425 = o[k] > n.hi[k];
            }
            else
            {
                _1425 = _1414;
            }
            if (_1425)
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
    precise float _1334 = c * d;
    float cd = _1334;
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
    bool _1532;
    if (false)
    {
        _1532 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _1532 = false;
    }
    bool _1539;
    if (_1532)
    {
        _1539 = !any(isnan(d));
    }
    else
    {
        _1539 = _1532;
    }
    bool _1546;
    if (_1539)
    {
        _1546 = !any(isinf(d));
    }
    else
    {
        _1546 = _1539;
    }
    bool topNonzero = _1546;
    firstN = 0.0f.xxx;
    firstA = 0.0f.xxx;
    firstE1 = 0.0f.xxx;
    firstE2 = 0.0f.xxx;
    bestT = farT;
    owner = 0u;
    bestPrimitive = 4294967295u;
    bool found = false;
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
    float3 _2097;
    while (tp != 0u)
    {
        uint _1562 = tp;
        uint _1563 = _1562 - uint(1);
        tp = _1563;
        uint at = ts[_1563];
        uint _1572;
        geometryTop.GetDimensions(_1572);
        _1572 = (_1572 - 0) / 48;
        if (at >= uint(int(_1572)))
        {
            return -1;
        }
        GeometryNode _1584;
        _1584.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _1584.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _1584.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _1584.lo;
        n.hi = _1584.hi;
        n.links = _1584.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _1605 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _1605;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _1617 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _1617;
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
            uint _1634 = tp;
            tp = _1634 + uint(1);
            ts[_1634] = n.links.x;
            uint _1639 = tp;
            tp = _1639 + uint(1);
            ts[_1639] = n.links.y;
            continue;
        }
        uint _1652;
        geometryInstances.GetDimensions(_1652);
        _1652 = (_1652 - 0) / 64;
        if (n.links.z >= uint(int(_1652)))
        {
            return -1;
        }
        GeometryInstance _1666;
        _1666.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64 + 0));
        _1666.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _1666.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _1666.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _1666.inverse0;
        inst.inverse1 = _1666.inverse1;
        inst.inverse2 = _1666.inverse2;
        inst.identity = _1666.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        bool _1720 = any(isnan(lo));
        bool _1727;
        if (!_1720)
        {
            _1727 = any(isinf(lo));
        }
        else
        {
            _1727 = _1720;
        }
        bool _1734;
        if (!_1727)
        {
            _1734 = any(isnan(ld));
        }
        else
        {
            _1734 = _1727;
        }
        bool _1741;
        if (!_1734)
        {
            _1741 = any(isinf(ld));
        }
        else
        {
            _1741 = _1734;
        }
        if (_1741)
        {
            return -1;
        }
        bool _1751;
        if (false)
        {
            _1751 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _1751 = false;
        }
        bool bottomNonzero = _1751;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _1764 = bp;
            uint _1765 = _1764 - uint(1);
            bp = _1765;
            at = bs[_1765];
            uint _1773;
            geometryBottom.GetDimensions(_1773);
            _1773 = (_1773 - 0) / 48;
            if (at >= uint(int(_1773)))
            {
                return -1;
            }
            GeometryNode _1783;
            _1783.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _1783.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1783.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _1783.lo;
            b.hi = _1783.hi;
            b.links = _1783.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _1804 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _1804;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _1816 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _1816;
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
                uint _1833 = bp;
                bp = _1833 + uint(1);
                bs[_1833] = b.links.x;
                uint _1838 = bp;
                bp = _1838 + uint(1);
                bs[_1838] = b.links.y;
                continue;
            }
            uint _1851;
            geometryTriangles.GetDimensions(_1851);
            _1851 = (_1851 - 0) / 48;
            bool _1854 = b.links.z > uint(int(_1851));
            bool _1867;
            if (!_1854)
            {
                uint _1860;
                geometryTriangles.GetDimensions(_1860);
                _1860 = (_1860 - 0) / 48;
                _1867 = b.links.w > (uint(int(_1860)) - b.links.z);
            }
            else
            {
                _1867 = _1854;
            }
            if (_1867)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _1890;
                _1890.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 0));
                _1890.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _1890.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _1890.a;
                tri.e1 = _1890.e1;
                tri.e2 = _1890.e2;
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
                bool _1976 = isnan(t);
                bool _1982;
                if (!_1976)
                {
                    _1982 = isinf(t);
                }
                else
                {
                    _1982 = _1976;
                }
                bool _1988;
                if (!_1982)
                {
                    _1988 = isnan(bary);
                }
                else
                {
                    _1988 = _1982;
                }
                bool _1994;
                if (!_1988)
                {
                    _1994 = isinf(bary);
                }
                else
                {
                    _1994 = _1988;
                }
                bool _2000;
                if (!_1994)
                {
                    _2000 = isnan(u);
                }
                else
                {
                    _2000 = _1994;
                }
                bool _2006;
                if (!_2000)
                {
                    _2006 = isinf(u);
                }
                else
                {
                    _2006 = _2000;
                }
                if (_2006)
                {
                    return -1;
                }
                bool _2011 = bary < 0.0f;
                bool _2019;
                if (!_2011)
                {
                    _2019 = (u + bary) > 1.0f;
                }
                else
                {
                    _2019 = _2011;
                }
                bool _2023 = _2019 || (t < nearT);
                bool _2030;
                if (!_2023)
                {
                    _2030 = t > bestT;
                }
                else
                {
                    _2030 = _2023;
                }
                if (_2030)
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _2044;
                if (found)
                {
                    _2044 = t == bestT;
                }
                else
                {
                    _2044 = found;
                }
                bool _2065;
                if (_2044)
                {
                    bool _2050 = inst.identity.y > owner;
                    bool _2064;
                    if (!_2050)
                    {
                        bool _2057 = inst.identity.y == owner;
                        bool _2063;
                        if (_2057)
                        {
                            _2063 = primitive >= bestPrimitive;
                        }
                        else
                        {
                            _2063 = _2057;
                        }
                        _2064 = _2063;
                    }
                    else
                    {
                        _2064 = _2050;
                    }
                    _2065 = _2064;
                }
                else
                {
                    _2065 = _2044;
                }
                if (_2065)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
                bool _2079 = all(bool4(inst.inverse0.x == float4(1.0f, 0.0f, 0.0f, 0.0f).x, inst.inverse0.y == float4(1.0f, 0.0f, 0.0f, 0.0f).y, inst.inverse0.z == float4(1.0f, 0.0f, 0.0f, 0.0f).z, inst.inverse0.w == float4(1.0f, 0.0f, 0.0f, 0.0f).w));
                bool _2087;
                if (_2079)
                {
                    _2087 = all(bool4(inst.inverse1.x == float4(0.0f, 1.0f, 0.0f, 0.0f).x, inst.inverse1.y == float4(0.0f, 1.0f, 0.0f, 0.0f).y, inst.inverse1.z == float4(0.0f, 1.0f, 0.0f, 0.0f).z, inst.inverse1.w == float4(0.0f, 1.0f, 0.0f, 0.0f).w));
                }
                else
                {
                    _2087 = _2079;
                }
                bool _2095;
                if (_2087)
                {
                    _2095 = all(bool4(inst.inverse2.x == float4(0.0f, 0.0f, 1.0f, 0.0f).x, inst.inverse2.y == float4(0.0f, 0.0f, 1.0f, 0.0f).y, inst.inverse2.z == float4(0.0f, 0.0f, 1.0f, 0.0f).z, inst.inverse2.w == float4(0.0f, 0.0f, 1.0f, 0.0f).w));
                }
                else
                {
                    _2095 = _2087;
                }
                bool identity = _2095;
                if (identity)
                {
                    _2097 = normalize(normal);
                }
                else
                {
                    _2097 = 0.0f.xxx;
                }
                firstN = _2097;
                firstA = tri.a.xyz;
                firstE1 = tri.e1.xyz;
                firstE2 = tri.e2.xyz;
            }
        }
    }
    return int(found);
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
    bool _857 = g.x < 0.0f;
    bool _864;
    if (!_857)
    {
        _864 = g.y < 0.0f;
    }
    else
    {
        _864 = _857;
    }
    bool _871;
    if (!_864)
    {
        _871 = g.z < 0.0f;
    }
    else
    {
        _871 = _864;
    }
    if (_871)
    {
        return 4294967295u;
    }
    uint3 c = uint3(g);
    bool _883 = c.x >= gp.dims.x;
    bool _892;
    if (!_883)
    {
        _892 = c.y >= gp.dims.y;
    }
    else
    {
        _892 = _883;
    }
    bool _901;
    if (!_892)
    {
        _901 = c.z >= gp.dims.z;
    }
    else
    {
        _901 = _892;
    }
    if (_901)
    {
        return 4294967295u;
    }
    return c.x + (gp.dims.x * (c.y + (gp.dims.y * c.z)));
}

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _560;
    if (axis == 0u)
    {
        _560 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _568;
        if (axis == 1u)
        {
            _568 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _568 = float3(0.0f, 0.0f, s);
        }
        _560 = _568;
    }
    return _560;
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
        float _415;
        if (axis == 0u)
        {
            _415 = card.extent.y;
        }
        else
        {
            float _423;
            if (axis == 1u)
            {
                _423 = card.extent.x;
            }
            else
            {
                _423 = card.extent.x;
            }
            _415 = _423;
        }
        float eu = _415;
        float _436;
        if (axis == 0u)
        {
            _436 = card.extent.z;
        }
        else
        {
            float _444;
            if (axis == 1u)
            {
                _444 = card.extent.z;
            }
            else
            {
                _444 = card.extent.y;
            }
            _436 = _444;
        }
        float ev = _436;
        float _457;
        if (eu > 9.9999997473787516355514526367188e-05f)
        {
            _457 = pad_m / (eu * 2.0f);
        }
        else
        {
            _457 = 0.0f;
        }
        padU = _457;
        float _468;
        if (ev > 9.9999997473787516355514526367188e-05f)
        {
            _468 = pad_m / (ev * 2.0f);
        }
        else
        {
            _468 = 0.0f;
        }
        padV = _468;
    }
    bool _482 = u < ((-margin_uv) - padU);
    bool _492;
    if (!_482)
    {
        _492 = u >= ((1.0f + margin_uv) + padU);
    }
    else
    {
        _492 = _482;
    }
    bool _502;
    if (!_492)
    {
        _502 = v < ((-margin_uv) - padV);
    }
    else
    {
        _502 = _492;
    }
    bool _512;
    if (!_502)
    {
        _512 = v >= ((1.0f + margin_uv) + padV);
    }
    else
    {
        _512 = _502;
    }
    if (_512)
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999998986721038818359375f);
    v = clamp(v, 0.0f, 0.999998986721038818359375f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card)
{
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5f) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5f) / float(card.resolution);
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float3 local_pos;
    if (axis == 0u)
    {
        local_pos.x = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
        local_pos.y = ((u - 0.5f) * card.extent.y) * 2.0f;
        local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
    }
    else
    {
        if (axis == 1u)
        {
            local_pos.y = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
        }
        else
        {
            local_pos.z = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.y = ((v - 0.5f) * card.extent.y) * 2.0f;
        }
    }
    return local_pos + card.center.xyz;
}

bool scQueryCardCandidateDbg(SurfaceCacheCard card, float3 hitPos, float3 surfaceFacing, float tolFloor, float qpadM, Texture2D<float4> depthAtlas, Texture2D<float4> lightingAtlas, float3 hitNormalHint, out float3 outRadiance, out float outKey, out uint outReject)
{
    outRadiance = 0.0f.xxx;
    outKey = 1000000015047466219876688855040.0f;
    outReject = 0u;
    float3 param = hitPos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 1.0f / float(max(card.resolution, 1u));
    float param_4 = qpadM;
    float2 param_2;
    bool _596 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    float2 atlasUV = param_2;
    if (!_596)
    {
        outReject = 1u;
        return false;
    }
    SurfaceCacheCard param_5 = card;
    if (dot(scCardFaceNormal(param_5), surfaceFacing) >= 0.0f)
    {
        outReject = 2u;
        return false;
    }
    float hintPenalty = 0.0f;
    if (dot(hitNormalHint, hitNormalHint) > 0.25f)
    {
        SurfaceCacheCard param_6 = card;
        float d = dot(scCardFaceNormal(param_6), hitNormalHint);
        if (d > (-0.5f))
        {
            outReject = 7u;
        }
        hintPenalty = max(d + 1.0f, 0.0f);
    }
    uint2 texel = uint2(clamp(atlasUV, 0.0f.xx, float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth = depthAtlas.Load(int3(int2(texel), 0)).x;
    bool _662 = isnan(depth);
    bool _668;
    if (!_662)
    {
        _668 = isinf(depth);
    }
    else
    {
        _668 = _662;
    }
    if (_668 || (depth < 0.0f))
    {
        outReject = 3u;
        return false;
    }
    uint2 param_7 = texel;
    float param_8 = depth;
    SurfaceCacheCard param_9 = card;
    float3 captured = cardTexelToWorld(param_7, param_8, param_9);
    float3 delta = captured - hitPos;
    SurfaceCacheCard param_10 = card;
    float3 n = scCardFaceNormal(param_10);
    float along = dot(delta, n);
    float3 perpV = delta - (n * along);
    float perp = length(perpV);
    float footprint = length(card.extent.xyz) / float(max(card.resolution, 1u));
    float tolAlong = tolFloor + footprint;
    float tolPerp = max(footprint * 1.5f, tolFloor);
    if (abs(along) > tolAlong)
    {
        outReject = 4u;
        return false;
    }
    if (perp > tolPerp)
    {
        outReject = 5u;
        return false;
    }
    float3 c = lightingAtlas.Load(int3(int2(texel), 0)).xyz;
    bool _749 = any(isnan(c));
    bool _756;
    if (!_749)
    {
        _756 = any(isinf(c));
    }
    else
    {
        _756 = _749;
    }
    if (_756)
    {
        outReject = 6u;
        return false;
    }
    outRadiance = c;
    outKey = (perp + (0.5f * abs(along))) + (hintPenalty * ((perp + abs(along)) + max(tolFloor, 0.0500000007450580596923828125f)));
    return true;
}

bool zeroTrianglePoint(float3 p, float3 a, float3 e1, float3 e2, float3 n)
{
    if (abs(dot(p - a, n)) > 0.00200000009499490261077880859375f)
    {
        return false;
    }
    float3 v = p - a;
    float aa = dot(e1, e1);
    float bb = dot(e2, e2);
    float ab = dot(e1, e2);
    float param = aa;
    float param_1 = bb;
    float param_2 = ab;
    float param_3 = ab;
    float det = geometryDifferenceOfProducts(param, param_1, param_2, param_3);
    if (det <= 9.9999999600419720025001879548654e-13f)
    {
        return false;
    }
    float u = ((dot(v, e1) * bb) - (dot(v, e2) * ab)) / det;
    float w = ((dot(v, e2) * aa) - (dot(v, e1) * ab)) / det;
    bool _2194 = (u >= 0.0f) && (w >= 0.0f);
    bool _2201;
    if (_2194)
    {
        _2201 = (u + w) <= 1.0f;
    }
    else
    {
        _2201 = _2194;
    }
    return _2201;
}

float3 wrcSampleRadiance(float3 stopPos, float3 dir, out bool ok)
{
    ok = false;
    if (global_wrc_fb[1].w <= 0.5f)
    {
        return 0.0f.xxx;
    }
    float3 org = global_wrc_fb[0].xyz;
    float spacing = max(global_wrc_fb[0].w, 0.00999999977648258209228515625f);
    int3 counts = int3(global_wrc_fb[1].xyz);
    float3 gp = (stopPos - org) / spacing.xxx;
    float3 gridMax = float3(counts - int3(1, 1, 1));
    bool _2252 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _2260;
    if (!_2252)
    {
        _2260 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _2260 = _2252;
    }
    if (_2260)
    {
        return 0.0f.xxx;
    }
    int3 c0 = clamp(int3(round(gp)), int3(0, 0, 0), counts - int3(1, 1, 1));
    uint pidx = uint(c0.x) + (uint(counts.x) * (uint(c0.y) + (uint(counts.y) * uint(c0.z))));
    uint rbase = pidx * 128u;
    uint metaBase = (((uint(counts.x) * uint(counts.y)) * uint(counts.z)) * 128u) + (pidx * 4u);
    uint state = _2317.Load((metaBase + 2u) * 4 + 0);
    bool _2325 = (state & 1u) == 0u;
    bool _2332;
    if (!_2325)
    {
        _2332 = (state & 2u) != 0u;
    }
    else
    {
        _2332 = _2325;
    }
    if (_2332)
    {
        return 0.0f.xxx;
    }
    float3 acc = 0.0f.xxx;
    float wSum = 0.0f;
    for (uint k = 0u; k < 64u; k++)
    {
        float y = 1.0f - ((2.0f * (float(k) + 0.5f)) / 64.0f);
        float r = sqrt(max(1.0f - (y * y), 0.0f));
        float a = 2.3999631404876708984375f * float(k);
        float3 wk = float3(cos(a) * r, y, sin(a) * r);
        float c = dot(wk, dir);
        if (c <= 0.25f)
        {
            continue;
        }
        float w = ((((c * c) * c) * c) * c) * c;
        uint2 p = uint2(_2317.Load((rbase + (k * 2u)) * 4 + 0), _2317.Load(((rbase + (k * 2u)) + 1u) * 4 + 0));
        float2 lo = spvUnpackHalf2x16(p.x);
        float2 hi = spvUnpackHalf2x16(p.y);
        acc += (float3(lo.x, lo.y, hi.x) * w);
        wSum += w;
    }
    if (wSum <= 0.0f)
    {
        return 0.0f.xxx;
    }
    float3 r_1 = acc / wSum.xxx;
    bool _2449 = any(isnan(r_1));
    bool _2456;
    if (!_2449)
    {
        _2456 = any(isinf(r_1));
    }
    else
    {
        _2456 = _2449;
    }
    if (_2456)
    {
        return 0.0f.xxx;
    }
    ok = true;
    uint _2466;
    _2463.InterlockedAdd(pidx * 4 + 0, 1u, _2466);
    return min(r_1, 64.0f.xxx);
}

float3 EvaluateSkyBase(float3 dir)
{
    float up = clamp(dir.y, 0.0f, 1.0f);
    float3 horizonCol = float3(4.400000095367431640625f, 4.19999980926513671875f, 3.7999999523162841796875f);
    float3 zenithCol = float3(5.5f, 5.7600002288818359375f, 6.0f);
    return lerp(horizonCol, zenithCol, pow(up, 0.449999988079071044921875f).xxx);
}

void spgiStatRecord(uint probeIdx, uint branch, float3 radiance, float cosTheta)
{
    if (global_diag_params.x < 0.5f)
    {
        return;
    }
    uint gridW = max(uint(global_grid_params.x), 1u);
    uint gridH = max(uint(global_grid_params.y), 1u);
    float lum = dot(radiance, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    uint lumQ = uint(clamp(lum, 0.0f, 65535.0f) * 64.0f);
    uint _2602;
    if (branch == 6u)
    {
        _2602 = uint(clamp(cosTheta, 0.0f, 4.0f) * 16.0f);
    }
    else
    {
        _2602 = uint(clamp(cosTheta, 0.0f, 1.0f) * 32.0f);
    }
    uint cosQ = _2602;
    uint col = min((((probeIdx % gridW) * 4u) / gridW), 3u);
    uint row = min((((probeIdx / gridW) * 4u) / gridH), 3u);
    uint bucket = (1u + col) + (row * 4u);
    uint _2660;
    _2657.InterlockedAdd(branch * 272 + 0, 1u, _2660);
    uint _2664;
    _2657.InterlockedAdd(branch * 272 + 4, lumQ, _2664);
    uint _2668;
    _2657.InterlockedMax(branch * 272 + 8, lumQ, _2668);
    uint _2672;
    _2657.InterlockedAdd(branch * 272 + 12, cosQ, _2672);
    uint _2676;
    _2657.InterlockedAdd(branch * 272 + bucket * 16 + 0, 1u, _2676);
    uint _2681;
    _2657.InterlockedAdd(branch * 272 + bucket * 16 + 4, lumQ, _2681);
    uint _2686;
    _2657.InterlockedMax(branch * 272 + bucket * 16 + 8, lumQ, _2686);
    uint _2691;
    _2657.InterlockedAdd(branch * 272 + bucket * 16 + 12, cosQ, _2691);
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
            probeRadiance.Store4(global_id * 16 + 0, asuint(float4(0.0f, 0.0f, 0.0f, -1.0f)));
            return;
        }
        rayIdx = global_id % raysPerProbe;
        bool _2744 = global_tier_params.w > 0.5f;
        bool _2757;
        if (_2744)
        {
            _2757 = float(rayIdx) >= asfloat(probeNormals.Load(probeIdx * 16 + 12));
        }
        else
        {
            _2757 = _2744;
        }
        if (_2757)
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
    bool measuredVisibility = global_tier_counts.w > 0.5f;
    uint zeroFlags = uint(global_surface_cache_params.w);
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
        float3 _2879 = spgiImportanceDirection(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
        invNq = param_9;
        rayDir = _2879;
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
    float4 hit = asfloat(hit_buffer.Load4(global_id * 16 + 0));
    bool _2929;
    if (measuredVisibility)
    {
        _2929 = hit.w == 3.0f;
    }
    else
    {
        _2929 = measuredVisibility;
    }
    if (_2929)
    {
        hit = float4(-1.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 radiance = 0.0f.xxx;
    float cosTheta = 0.0f;
    bool useWorldFallback = false;
    uint scRejectReason = 5u;
    uint auditEvalCount = 0u;
    uint auditBounds = 0u;
    uint auditOrient = 0u;
    uint auditDepth = 0u;
    uint auditCardSlot = 4294967295u;
    bool _2944 = global_diag_params.x > 5.5f;
    bool _2951;
    if (_2944)
    {
        _2951 = global_diag_params.x < 6.5f;
    }
    else
    {
        _2951 = _2944;
    }
    if (_2951)
    {
        uint px = probeIdx % gridW;
        uint py = probeIdx / gridW;
        uint _2966;
        if (px == 260u)
        {
            _2966 = 0u;
        }
        else
        {
            uint _2973;
            if (px == 285u)
            {
                _2973 = 1u;
            }
            else
            {
                _2973 = (px == 310u) ? 2u : 3u;
            }
            _2966 = _2973;
        }
        uint sx = _2966;
        uint _2987;
        if (py == 82u)
        {
            _2987 = 0u;
        }
        else
        {
            uint _2994;
            if (py == 112u)
            {
                _2994 = 1u;
            }
            else
            {
                _2994 = (py == 138u) ? 2u : 3u;
            }
            _2987 = _2994;
        }
        uint sy = _2987;
        if (global_hint_params.y > 0.5f)
        {
            uint _3013;
            if (px == 110u)
            {
                _3013 = 0u;
            }
            else
            {
                uint _3020;
                if (px == 160u)
                {
                    _3020 = 1u;
                }
                else
                {
                    _3020 = (px == 210u) ? 2u : 3u;
                }
                _3013 = _3020;
            }
            sx = _3013;
            uint _3032;
            if (py == 160u)
            {
                _3032 = 0u;
            }
            else
            {
                uint _3039;
                if (py == 168u)
                {
                    _3039 = 1u;
                }
                else
                {
                    _3039 = (py == 176u) ? 2u : 3u;
                }
                _3032 = _3039;
            }
            sy = _3032;
        }
        uint _3052;
        if (rayIdx == 0u)
        {
            _3052 = 0u;
        }
        else
        {
            uint _3059;
            if (rayIdx == 12u)
            {
                _3059 = 1u;
            }
            else
            {
                uint _3066;
                if (rayIdx == 24u)
                {
                    _3066 = 2u;
                }
                else
                {
                    _3066 = (rayIdx == 36u) ? 3u : 4u;
                }
                _3059 = _3066;
            }
            _3052 = _3059;
        }
        uint sr = _3052;
        if (((sx < 3u) && (sy < 3u)) && (sr < 4u))
        {
            auditCardSlot = (((sy * 3u) + sx) * 4u) + sr;
        }
    }
    uint statBranch = 3u;
    if (hit.w == 1.0f)
    {
        float2 param_18 = hit.yz;
        radiance = samplePrevFrameColorAt(param_18);
        bool _3105 = any(isnan(radiance));
        bool _3112;
        if (!_3105)
        {
            _3112 = any(isinf(radiance));
        }
        else
        {
            _3112 = _3105;
        }
        if (_3112)
        {
            radiance = 0.0f.xxx;
        }
        float hzbHitLum = dot(radiance, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        bool _3120 = global_diag_params.x < 1.5f;
        bool _3128;
        if (!_3120)
        {
            _3128 = global_diag_params.x > 2.5f;
        }
        else
        {
            _3128 = _3120;
        }
        if (_3128)
        {
            radiance *= (1.0f / (1.0f + (max(hzbHitLum - global_tier_counts.y, 0.0f) * 2.0f)));
        }
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
        statBranch = 0u;
    }
    else
    {
        if (hit.w == 2.0f)
        {
            float3 hitPos = origin + (rayDir * hit.x);
            float3 hitNormalHint = 0.0f.xxx;
            float2 octS = spvUnpackSnorm2x16(asuint(hit.y));
            float2 param_19 = clamp((octS * 0.5f) + 0.5f.xx, 0.0f.xx, 1.0f.xx);
            float3 _3172 = octDecode(param_19);
            float3 decoded = _3172;
            if (dot(decoded, decoded) > 0.25f)
            {
                hitNormalHint = decoded;
            }
            float faceGate = global_diag_params.y;
            bool usedSurfaceCache = false;
            uint zeroOwner = 4294967295u;
            uint zeroPrimitive = 4294967295u;
            float3 zeroN = 0.0f.xxx;
            float3 zeroA = 0.0f.xxx;
            float3 zeroE1 = 0.0f.xxx;
            float3 zeroE2 = 0.0f.xxx;
            uint zeroStatus = 0u;
            float firstT = -1.0f;
            float param_24;
            uint param_25;
            uint param_26;
            float3 param_27;
            float3 param_28;
            float3 param_29;
            float3 param_30;
            uint _3249;
            uint _3523;
            uint _3539;
            uint _3565;
            float3 _3581;
            SurfaceCacheCard queryCard;
            float2 param_39;
            float3 param_48;
            float param_49;
            uint param_50;
            float2 param_53;
            float2 param_62;
            for (uint zeroAttempt = 0u; zeroAttempt < 3u; zeroAttempt++)
            {
                if (zeroAttempt == 1u)
                {
                    bool _3211;
                    if (!usedSurfaceCache)
                    {
                        _3211 = (zeroFlags & 1u) == 0u;
                    }
                    else
                    {
                        _3211 = usedSurfaceCache;
                    }
                    bool _3218;
                    if (!_3211)
                    {
                        _3218 = hit.x != 0.0f;
                    }
                    else
                    {
                        _3218 = _3211;
                    }
                    if (_3218)
                    {
                        break;
                    }
                    zeroStatus = 1u;
                    float3 param_20 = origin;
                    float3 param_21 = rayDir;
                    float param_22 = 1.9999999494757503271102905273438e-05f;
                    float param_23 = global_trace_params.x;
                    int _3239 = geometryFirstHit(param_20, param_21, param_22, param_23, param_24, param_25, param_26, param_27, param_28, param_29, param_30);
                    firstT = param_24;
                    zeroOwner = param_25;
                    zeroPrimitive = param_26;
                    zeroN = param_27;
                    zeroA = param_28;
                    zeroE1 = param_29;
                    zeroE2 = param_30;
                    int first = _3239;
                    if (first < 0)
                    {
                        _3249 = 2u;
                    }
                    else
                    {
                        _3249 = (first == 0) ? 3u : 4u;
                    }
                    zeroStatus = _3249;
                    bool _3258 = first != 1;
                    bool _3266;
                    if (!_3258)
                    {
                        _3266 = dot(zeroN, zeroN) < 0.5f;
                    }
                    else
                    {
                        _3266 = _3258;
                    }
                    bool _3274;
                    if (!_3266)
                    {
                        _3274 = dot(zeroN, rayDir) >= 0.0f;
                    }
                    else
                    {
                        _3274 = _3266;
                    }
                    bool _3287;
                    if (!_3274)
                    {
                        uint _3283;
                        zeroOwners.GetDimensions(_3283);
                        _3283 = (_3283 - 0) / 16;
                        _3287 = zeroOwner >= uint(int(_3283));
                    }
                    else
                    {
                        _3287 = _3274;
                    }
                    if (_3287)
                    {
                        break;
                    }
                    uint4 own = zeroOwners.Load4(zeroOwner * 16 + 0);
                    bool _3298 = own.y != 1u;
                    bool _3306;
                    if (!_3298)
                    {
                        _3306 = own.z != zeroOwner;
                    }
                    else
                    {
                        _3306 = _3298;
                    }
                    bool _3316;
                    if (!_3306)
                    {
                        _3316 = own.w != uint(global_camera_position.w);
                    }
                    else
                    {
                        _3316 = _3306;
                    }
                    bool _3331;
                    if (!_3316)
                    {
                        uint _3326;
                        zeroMaterials.GetDimensions(_3326);
                        _3326 = (_3326 - 0) / 16;
                        _3331 = own.x >= (uint(int(_3326)) / 3u);
                    }
                    else
                    {
                        _3331 = _3316;
                    }
                    if (_3331)
                    {
                        zeroStatus = 5u;
                        break;
                    }
                    if (zeroMaterials.Load(((own.x * 3u) + 2u) * 16 + 12) != 0u)
                    {
                        zeroStatus = 6u;
                        break;
                    }
                    bool _3347 = global_cache_params.x > 0.0f;
                    bool _3354;
                    if (_3347)
                    {
                        _3354 = firstT > global_cache_params.x;
                    }
                    else
                    {
                        _3354 = _3347;
                    }
                    if (_3354)
                    {
                        zeroStatus = 7u;
                        break;
                    }
                    hitPos = origin + (rayDir * firstT);
                    hitNormalHint = zeroN;
                    zeroStatus = 8u;
                }
                if (zeroAttempt == 2u)
                {
                    bool _3371 = usedSurfaceCache || (zeroStatus != 8u);
                    bool _3378;
                    if (!_3371)
                    {
                        _3378 = (zeroFlags & 4u) == 0u;
                    }
                    else
                    {
                        _3378 = _3371;
                    }
                    if (_3378)
                    {
                        break;
                    }
                    uint _3387;
                    cardLookup.GetDimensions(_3387);
                    _3387 = (_3387 - 0) / 4;
                    if (zeroOwner >= (uint(int(_3387)) / 12u))
                    {
                        zeroStatus = 12u;
                        break;
                    }
                    uint db = cardLookup.Load(((zeroOwner * 12u) + 10u) * 4 + 0);
                    uint de = cardLookup.Load(((zeroOwner * 12u) + 11u) * 4 + 0);
                    bool _3411 = db > de;
                    bool _3425;
                    if (!_3411)
                    {
                        uint _3421;
                        cards.GetDimensions(_3421);
                        _3421 = (_3421 - 0) / 64;
                        _3425 = de > uint(int(_3421));
                    }
                    else
                    {
                        _3425 = _3411;
                    }
                    bool _3434;
                    if (!_3425)
                    {
                        _3434 = de > uint(global_surface_cache_params.y);
                    }
                    else
                    {
                        _3434 = _3425;
                    }
                    if (_3434)
                    {
                        zeroStatus = 12u;
                        break;
                    }
                    zeroStatus = 10u;
                }
                bool _3440 = global_surface_cache_params.z > 0.5f;
                bool _3452;
                if (_3440)
                {
                    uint _3445_dummy_parameter;
                    int2 _3445 = int2(spvTextureSize(surface_cache_depth, uint(0), _3445_dummy_parameter));
                    uint _3448_dummy_parameter;
                    int2 _3448 = int2(spvTextureSize(surface_cache_lighting, uint(0), _3448_dummy_parameter));
                    _3452 = all(bool2(_3445.x == _3448.x, _3445.y == _3448.y));
                }
                else
                {
                    _3452 = _3440;
                }
                if (_3452)
                {
                    uint4 gh0 = uint4(scgo.Load(0), scgo.Load(4), scgo.Load(8), scgo.Load(12));
                    uint4 gh1 = uint4(scgo.Load(16), scgo.Load(20), scgo.Load(24), scgo.Load(28));
                    uint4 gh2 = uint4(scgo.Load(32), scgo.Load(36), scgo.Load(40), scgo.Load(44));
                    uint4 param_31 = gh0;
                    uint4 param_32 = gh1;
                    uint4 param_33 = gh2;
                    SCGridParams gp = scGridDecode(param_31, param_32, param_33);
                    SCGridParams param_34 = gp;
                    float3 param_35 = hitPos;
                    uint cell = scGridCellIndex(param_34, param_35);
                    float bestKey = 1000000015047466219876688855040.0f;
                    bool anyAccept = false;
                    bool anyOrient = false;
                    bool anyBounds = false;
                    bool anyDepth = false;
                    uint evalCount = 0u;
                    bool ownerRetry = zeroAttempt == 2u;
                    if (ownerRetry || (cell != 4294967295u))
                    {
                        if (ownerRetry)
                        {
                            _3523 = cardLookup.Load(((zeroOwner * 12u) + 10u) * 4 + 0);
                        }
                        else
                        {
                            _3523 = scgo.Load((16u + cell) * 4 + 0);
                        }
                        uint begin = _3523;
                        if (ownerRetry)
                        {
                            _3539 = cardLookup.Load(((zeroOwner * 12u) + 11u) * 4 + 0);
                        }
                        else
                        {
                            _3539 = scgo.Load((17u + cell) * 4 + 0);
                        }
                        uint end = _3539;
                        for (uint gi = begin; gi < end; gi++)
                        {
                            if (ownerRetry)
                            {
                                _3565 = gi;
                            }
                            else
                            {
                                _3565 = scgc.Load(gi * 4 + 0);
                            }
                            uint ci = _3565;
                            if (faceGate < 0.0f)
                            {
                                _3581 = 0.0f.xxx;
                            }
                            else
                            {
                                _3581 = hitNormalHint * (0.5f / max(faceGate, 0.001000000047497451305389404296875f));
                            }
                            float3 hintForQuery = _3581;
                            bool hintSoft = global_tier_counts.z > 0.5f;
                            uint scReject = 0u;
                            uint _3602;
                            _3600.InterlockedAdd(0, 1u, _3602);
                            evalCount++;
                            SurfaceCacheCard _3609;
                            _3609.center = asfloat(cards.Load4(ci * 64 + 0));
                            _3609.extent = asfloat(cards.Load4(ci * 64 + 16));
                            _3609.axis_direction = cards.Load(ci * 64 + 32);
                            _3609.resolution = cards.Load(ci * 64 + 36);
                            _3609.atlas_offset_x = cards.Load(ci * 64 + 40);
                            _3609.atlas_offset_y = cards.Load(ci * 64 + 44);
                            _3609.mesh_instance_id = cards.Load(ci * 64 + 48);
                            _3609.static_mesh_slot = cards.Load(ci * 64 + 52);
                            _3609.flags = cards.Load(ci * 64 + 56);
                            _3609.page_card = cards.Load(ci * 64 + 60);
                            queryCard.center = _3609.center;
                            queryCard.extent = _3609.extent;
                            queryCard.axis_direction = _3609.axis_direction;
                            queryCard.resolution = _3609.resolution;
                            queryCard.atlas_offset_x = _3609.atlas_offset_x;
                            queryCard.atlas_offset_y = _3609.atlas_offset_y;
                            queryCard.mesh_instance_id = _3609.mesh_instance_id;
                            queryCard.static_mesh_slot = _3609.static_mesh_slot;
                            queryCard.flags = _3609.flags;
                            queryCard.page_card = _3609.page_card;
                            bool _3631 = zeroAttempt >= 1u;
                            bool _3638;
                            if (_3631)
                            {
                                _3638 = queryCard.mesh_instance_id != zeroOwner;
                            }
                            else
                            {
                                _3638 = _3631;
                            }
                            if (_3638)
                            {
                                continue;
                            }
                            float queryTol = max(0.1500000059604644775390625f, global_sdf_voxel_sizes[0].x * 1.25f);
                            float queryPad = min(queryTol, 0.449999988079071044921875f);
                            float3 queryFacing = -rayDir;
                            float backsideTransport = 1.0f;
                            bool transmissiveBackface = false;
                            SurfaceCacheCard param_36 = queryCard;
                            if (dot(scCardFaceNormal(param_36), queryFacing) >= 0.0f)
                            {
                                float3 param_37 = hitPos;
                                SurfaceCacheCard param_38 = queryCard;
                                float param_40 = 1.0f / float(max(queryCard.resolution, 1u));
                                float param_41 = queryPad;
                                bool _3679 = worldToCardUVMarginPad(param_37, param_38, param_39, param_40, param_41);
                                float2 materialUV = param_39;
                                if (_3679)
                                {
                                    uint2 materialTexel = uint2(clamp(materialUV, 0.0f.xx, float2(float((queryCard.atlas_offset_x + queryCard.resolution) - 1u), float((queryCard.atlas_offset_y + queryCard.resolution) - 1u))));
                                    uint transportBits = uint(round(clamp(surface_cache_albedo.Load(int3(int2(materialTexel), 0)).w, 0.0f, 1.0f) * 255.0f));
                                    float transmission = float((transportBits >> 3u) & 15u) / 15.0f;
                                    float thickness = float(transportBits & 7u) / 7.0f;
                                    backsideTransport = transmission * exp((-2.0f) * thickness);
                                    transmissiveBackface = ((transportBits & 128u) != 0u) && (backsideTransport > 0.0039215688593685626983642578125f);
                                }
                                if (transmissiveBackface)
                                {
                                    queryFacing = rayDir;
                                }
                            }
                            SurfaceCacheCard param_42 = queryCard;
                            float3 param_43 = hitPos;
                            float3 param_44 = queryFacing;
                            float param_45 = queryTol;
                            float param_46 = queryPad;
                            float3 param_47 = hintForQuery;
                            bool _3762 = scQueryCardCandidateDbg(param_42, param_43, param_44, param_45, param_46, surface_cache_depth, surface_cache_lighting, param_47, param_48, param_49, param_50);
                            float3 candidate = param_48;
                            float key = param_49;
                            scReject = param_50;
                            bool candidateAccepted = _3762;
                            bool _3767 = auditCardSlot != 4294967295u;
                            bool _3774;
                            if (_3767)
                            {
                                _3774 = (gi - begin) < 8u;
                            }
                            else
                            {
                                _3774 = _3767;
                            }
                            if (_3774)
                            {
                                _2657.Store4(auditCardSlot * 128 + (gi - begin) * 16 + 4208, uint4(ci, scReject, queryCard.mesh_instance_id, queryCard.axis_direction | (candidateAccepted ? 65536u : 0u)));
                                float4 metrics = (-1.0f).xxxx;
                                float3 param_51 = hitPos;
                                SurfaceCacheCard param_52 = queryCard;
                                float param_54 = 1.0f / float(max(queryCard.resolution, 1u));
                                float param_55 = queryPad;
                                bool _3809 = worldToCardUVMarginPad(param_51, param_52, param_53, param_54, param_55);
                                float2 auditUV = param_53;
                                if (_3809)
                                {
                                    uint2 auditTexel = uint2(clamp(auditUV, 0.0f.xx, float2(float((queryCard.atlas_offset_x + queryCard.resolution) - 1u), float((queryCard.atlas_offset_y + queryCard.resolution) - 1u))));
                                    float capturedDepth = surface_cache_depth.Load(int3(int2(auditTexel), 0)).x;
                                    bool _3840 = !isnan(capturedDepth);
                                    bool _3846;
                                    if (_3840)
                                    {
                                        _3846 = !isinf(capturedDepth);
                                    }
                                    else
                                    {
                                        _3846 = _3840;
                                    }
                                    if (_3846 && (capturedDepth >= 0.0f))
                                    {
                                        uint2 param_56 = auditTexel;
                                        float param_57 = capturedDepth;
                                        SurfaceCacheCard param_58 = queryCard;
                                        float3 delta = cardTexelToWorld(param_56, param_57, param_58) - hitPos;
                                        SurfaceCacheCard param_59 = queryCard;
                                        float3 face = scCardFaceNormal(param_59);
                                        float along = dot(delta, face);
                                        float footprint = length(queryCard.extent.xyz) / float(max(queryCard.resolution, 1u));
                                        metrics = float4(capturedDepth, along, length(delta - (face * along)), queryTol + footprint);
                                    }
                                }
                                _2657.Store4(auditCardSlot * 128 + (gi - begin) * 16 + 8816, asuint(metrics));
                            }
                            if (candidateAccepted && (zeroAttempt >= 1u))
                            {
                                float3 param_60 = hitPos;
                                SurfaceCacheCard param_61 = queryCard;
                                float param_63 = 1.0f / float(max(queryCard.resolution, 1u));
                                float param_64 = queryPad;
                                bool _3918 = worldToCardUVMarginPad(param_60, param_61, param_62, param_63, param_64);
                                float2 uv = param_62;
                                candidateAccepted = _3918;
                                if (candidateAccepted)
                                {
                                    int2 px_1 = int2(clamp(uv, 0.0f.xx, float2(float((queryCard.atlas_offset_x + queryCard.resolution) - 1u), float((queryCard.atlas_offset_y + queryCard.resolution) - 1u))));
                                    float d = surface_cache_depth.Load(int3(px_1, 0)).x;
                                    uint2 param_65 = uint2(px_1);
                                    float param_66 = d;
                                    SurfaceCacheCard param_67 = queryCard;
                                    float3 p = cardTexelToWorld(param_65, param_66, param_67);
                                    uint transport = uint(round(clamp(surface_cache_albedo.Load(int3(px_1, 0)).w, 0.0f, 1.0f) * 255.0f));
                                    bool _3967 = (transport & 128u) != 0u;
                                    bool _3974;
                                    if (_3967)
                                    {
                                        _3974 = ((transport >> 3u) & 15u) == 0u;
                                    }
                                    else
                                    {
                                        _3974 = _3967;
                                    }
                                    bool _3977 = _3974 && (!transmissiveBackface);
                                    bool _3991;
                                    if (_3977)
                                    {
                                        float3 param_68 = p;
                                        float3 param_69 = zeroA;
                                        float3 param_70 = zeroE1;
                                        float3 param_71 = zeroE2;
                                        float3 param_72 = zeroN;
                                        _3991 = zeroTrianglePoint(param_68, param_69, param_70, param_71, param_72);
                                    }
                                    else
                                    {
                                        _3991 = _3977;
                                    }
                                    candidateAccepted = _3991;
                                }
                            }
                            if (candidateAccepted)
                            {
                                if (transmissiveBackface)
                                {
                                    candidate *= backsideTransport;
                                }
                                if (transmissiveBackface)
                                {
                                    uint _4005;
                                    _3600.InterlockedAdd(32, 1u, _4005);
                                }
                                if (scReject == 7u)
                                {
                                    uint _4011;
                                    _3600.InterlockedAdd(28, 1u, _4011);
                                    if (!hintSoft)
                                    {
                                        uint _4017;
                                        _3600.InterlockedAdd(8, 1u, _4017);
                                        anyOrient = true;
                                        auditOrient++;
                                        continue;
                                    }
                                }
                                if (key < bestKey)
                                {
                                    bestKey = key;
                                    radiance = candidate;
                                    usedSurfaceCache = true;
                                    if (zeroAttempt != 0u)
                                    {
                                        zeroStatus = (zeroAttempt == 2u) ? 11u : 9u;
                                    }
                                }
                            }
                            else
                            {
                                if (scReject == 1u)
                                {
                                    uint _4042;
                                    _3600.InterlockedAdd(4, 1u, _4042);
                                    anyBounds = true;
                                    auditBounds++;
                                }
                                else
                                {
                                    if (scReject == 2u)
                                    {
                                        uint _4051;
                                        _3600.InterlockedAdd(8, 1u, _4051);
                                        anyOrient = true;
                                        auditOrient++;
                                    }
                                    else
                                    {
                                        if (scReject == 7u)
                                        {
                                            uint _4060;
                                            _3600.InterlockedAdd(28, 1u, _4060);
                                            anyOrient = true;
                                            auditOrient++;
                                        }
                                        else
                                        {
                                            if (scReject >= 3u)
                                            {
                                                uint _4069;
                                                _3600.InterlockedAdd(12, 1u, _4069);
                                                anyDepth = true;
                                                auditDepth++;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        auditEvalCount = evalCount;
                        if (evalCount == 0u)
                        {
                            uint _4080;
                            _3600.InterlockedAdd(44, 1u, _4080);
                        }
                    }
                    else
                    {
                        uint _4083;
                        _3600.InterlockedAdd(44, 1u, _4083);
                    }
                    if (usedSurfaceCache)
                    {
                        scRejectReason = 0u;
                        uint _4088;
                        _3600.InterlockedAdd(16, 1u, _4088);
                        uint _4090;
                        _3600.InterlockedAdd(36, 1u, _4090);
                    }
                    else
                    {
                        if (evalCount > 0u)
                        {
                            if (anyOrient)
                            {
                                uint _4100;
                                _3600.InterlockedAdd(40, 1u, _4100);
                                scRejectReason = 1u;
                            }
                            else
                            {
                                if (anyBounds)
                                {
                                    uint _4106;
                                    _3600.InterlockedAdd(52, 1u, _4106);
                                    scRejectReason = 2u;
                                }
                                else
                                {
                                    if (anyDepth)
                                    {
                                        uint _4112;
                                        _3600.InterlockedAdd(56, 1u, _4112);
                                        scRejectReason = 3u;
                                    }
                                    else
                                    {
                                        scRejectReason = 5u;
                                    }
                                }
                            }
                        }
                        else
                        {
                            scRejectReason = 4u;
                        }
                    }
                }
            }
            if (!usedSurfaceCache)
            {
                useWorldFallback = true;
                if (global_diag_params.x > 0.5f)
                {
                    uint _4134;
                    _3600.InterlockedAdd(((hit.x <= (global_sdf_voxel_sizes[0].x * 0.5f)) ? 12u : 15u) * 4 + 0, 1u, _4134);
                }
            }
            if (global_diag_params.w < 0.5f)
            {
                float scLum = dot(radiance, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
                radiance *= (1.0f / (1.0f + (max(scLum - global_tier_counts.y, 0.0f) * 2.0f)));
            }
            bool _4155 = any(isnan(radiance));
            bool _4162;
            if (!_4155)
            {
                _4162 = any(isinf(radiance));
            }
            else
            {
                _4162 = _4155;
            }
            if (_4162)
            {
                radiance = 0.0f.xxx;
            }
            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
            statBranch = usedSurfaceCache ? 1u : 3u;
        }
        else
        {
            if (hit.w == 3.0f)
            {
                statBranch = 2u;
                float3 stopPos = origin + (rayDir * max(hit.x, 0.0f));
                bool ddgiOk = false;
                bool wrcOk = false;
                float3 param_73 = stopPos;
                float3 param_74 = rayDir;
                bool param_75;
                float3 _4193 = wrcSampleRadiance(param_73, param_74, param_75);
                wrcOk = param_75;
                float3 wrcRad = _4193;
                if (wrcOk)
                {
                    radiance = max(wrcRad, 0.0f.xxx);
                    cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                    ddgiOk = true;
                }
                bool _4205 = !ddgiOk;
                bool _4211;
                if (_4205)
                {
                    _4211 = global_cache_params.w > 0.5f;
                }
                else
                {
                    _4211 = _4205;
                }
                bool _4217;
                if (_4211)
                {
                    _4217 = global_ddgi_fb[1].w > 0.5f;
                }
                else
                {
                    _4217 = _4211;
                }
                if (_4217)
                {
                    float ddgiSpan = max(max(global_ddgi_fb[1].x, global_ddgi_fb[1].y), global_ddgi_fb[1].z) * global_ddgi_fb[0].w;
                    float cExtent = max(ddgiSpan * 2.0f, 1.0f);
                    float3 cOrg = global_ddgi_fb[0].xyz - (cExtent * 0.25f).xxx;
                    float cSpacing = cExtent / 32.0f;
                    float3 cgp = (stopPos - cOrg) / cSpacing.xxx;
                    bool _4256 = all(bool3(cgp.x >= 0.0f.xxx.x, cgp.y >= 0.0f.xxx.y, cgp.z >= 0.0f.xxx.z));
                    bool _4263;
                    if (_4256)
                    {
                        _4263 = all(bool3(cgp.x < 32.0f.xxx.x, cgp.y < 32.0f.xxx.y, cgp.z < 32.0f.xxx.z));
                    }
                    else
                    {
                        _4263 = _4256;
                    }
                    if (_4263)
                    {
                        int3 ct = clamp(int3(floor(cgp)), int3(0, 0, 0), int3(31, 31, 31));
                        float4 cL0 = radianceCascadeL0.Load(int4(ct, 0));
                        float3 cL1x = radianceCascadeL1x.Load(int4(ct, 0)).xyz;
                        float3 cL1y = radianceCascadeL1y.Load(int4(ct, 0)).xyz;
                        float3 cL1z = radianceCascadeL1z.Load(int4(ct, 0)).xyz;
                        bool _4300 = cL0.w > 0.5f;
                        bool _4307;
                        if (_4300)
                        {
                            _4307 = any(bool3(cL0.xyz.x > 0.0f.xxx.x, cL0.xyz.y > 0.0f.xxx.y, cL0.xyz.z > 0.0f.xxx.z));
                        }
                        else
                        {
                            _4307 = _4300;
                        }
                        if (_4307)
                        {
                            float3 irr = (((cL0.xyz * 0.2820949852466583251953125f) - ((cL1x * 0.48860299587249755859375f) * rayDir.y)) + ((cL1y * 0.48860299587249755859375f) * rayDir.z)) - ((cL1z * 0.48860299587249755859375f) * rayDir.x);
                            bool _4334 = any(isnan(irr));
                            bool _4341;
                            if (!_4334)
                            {
                                _4341 = any(isinf(irr));
                            }
                            else
                            {
                                _4341 = _4334;
                            }
                            if (_4341)
                            {
                                irr = 0.0f.xxx;
                            }
                            radiance = min(max(irr, 0.0f.xxx), 64.0f.xxx);
                            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                            ddgiOk = true;
                        }
                    }
                }
                bool _4352 = !ddgiOk;
                bool _4358;
                if (_4352)
                {
                    _4358 = global_ddgi_fb[1].w > 0.5f;
                }
                else
                {
                    _4358 = _4352;
                }
                if (_4358)
                {
                    float3 org = global_ddgi_fb[0].xyz;
                    float spacing = max(global_ddgi_fb[0].w, 0.00999999977648258209228515625f);
                    int3 counts = int3(global_ddgi_fb[1].xyz);
                    float3 gp_1 = (stopPos - org) / spacing.xxx;
                    float3 gridMax = float3(counts - int3(1, 1, 1));
                    bool _4387 = all(bool3(gp_1.x >= 0.0f.xxx.x, gp_1.y >= 0.0f.xxx.y, gp_1.z >= 0.0f.xxx.z));
                    bool _4394;
                    if (_4387)
                    {
                        _4394 = all(bool3(gp_1.x <= gridMax.x, gp_1.y <= gridMax.y, gp_1.z <= gridMax.z));
                    }
                    else
                    {
                        _4394 = _4387;
                    }
                    if (_4394)
                    {
                        float3 f = clamp(frac(gp_1), 0.0f.xxx, 1.0f.xxx);
                        int3 b0 = clamp(int3(floor(gp_1)), int3(0, 0, 0), counts - int3(1, 1, 1));
                        bool _4414 = f.x >= f.y;
                        bool _4422;
                        if (_4414)
                        {
                            _4422 = f.x >= f.z;
                        }
                        else
                        {
                            _4422 = _4414;
                        }
                        int _4423;
                        if (_4422)
                        {
                            _4423 = 0;
                        }
                        else
                        {
                            _4423 = (f.y >= f.z) ? 1 : 2;
                        }
                        int ax = _4423;
                        int ay;
                        if (ax == 0)
                        {
                            ay = (f.y >= f.z) ? 1 : 2;
                        }
                        else
                        {
                            if (ax == 1)
                            {
                                ay = (f.x >= f.z) ? 0 : 2;
                            }
                            else
                            {
                                ay = (f.x >= f.y) ? 0 : 1;
                            }
                        }
                        int az = (3 - ax) - ay;
                        float fa = f[ax];
                        float fb = f[ay];
                        float fc = f[az];
                        int3 c0 = b0;
                        int3 _4485 = ax.xxx;
                        int3 c1 = min((b0 + int3(bool3(_4485.x == int3(0, 1, 2).x, _4485.y == int3(0, 1, 2).y, _4485.z == int3(0, 1, 2).z))), (counts - int3(1, 1, 1)));
                        int3 _4496 = ay.xxx;
                        int3 c2 = min((c1 + int3(bool3(_4496.x == int3(0, 1, 2).x, _4496.y == int3(0, 1, 2).y, _4496.z == int3(0, 1, 2).z))), (counts - int3(1, 1, 1)));
                        int3 _4506 = az.xxx;
                        int3 c3 = min((c2 + int3(bool3(_4506.x == int3(0, 1, 2).x, _4506.y == int3(0, 1, 2).y, _4506.z == int3(0, 1, 2).z))), (counts - int3(1, 1, 1)));
                        float w0 = 1.0f - fa;
                        float w1 = fa - fb;
                        float w2 = fb - fc;
                        float w3 = fc;
                        int3 _4533[4] = { c0, c1, c2, c3 };
                        int3 cn[4] = _4533;
                        float _4541[4] = { w0, w1, w2, w3 };
                        float wn[4] = _4541;
                        float3 acc = 0.0f.xxx;
                        float wsum = 0.0f;
                        for (int i = 0; i < 4; i++)
                        {
                            if (wn[i] <= 0.0f)
                            {
                                continue;
                            }
                            uint pidx = uint(cn[i].x) + (uint(counts.x) * (uint(cn[i].y) + (uint(counts.y) * uint(cn[i].z))));
                            float3 sh0 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 0u) * 16 + 0)).xyz;
                            float3 sh1 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 1u) * 16 + 0)).xyz;
                            float3 sh2 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 2u) * 16 + 0)).xyz;
                            float3 sh3 = asfloat(ddgiIrr.Load4(((pidx * 9u) + 3u) * 16 + 0)).xyz;
                            float3 irr_1 = (((sh0 * 0.2820949852466583251953125f) - ((sh1 * 0.48860299587249755859375f) * rayDir.y)) + ((sh2 * 0.48860299587249755859375f) * rayDir.z)) - ((sh3 * 0.48860299587249755859375f) * rayDir.x);
                            acc += (max(irr_1, 0.0f.xxx) * wn[i]);
                            wsum += wn[i];
                        }
                        if (wsum > 0.0f)
                        {
                            radiance = acc / wsum.xxx;
                            bool _4660 = any(isnan(radiance));
                            bool _4667;
                            if (!_4660)
                            {
                                _4667 = any(isinf(radiance));
                            }
                            else
                            {
                                _4667 = _4660;
                            }
                            if (_4667)
                            {
                                radiance = 0.0f.xxx;
                            }
                            radiance = min(radiance, 64.0f.xxx);
                            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                            ddgiOk = true;
                        }
                    }
                }
                if (!ddgiOk)
                {
                    float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
                    if (skyWeight > 0.0f)
                    {
                        float3 param_76 = rayDir;
                        radiance = EvaluateSkyBase(param_76) * global_sky_params.xyz;
                        cosTheta = skyWeight;
                    }
                    else
                    {
                        bool servedF1a = false;
                        bool _4702 = global_sky_params.w > 0.0f;
                        bool _4708;
                        if (_4702)
                        {
                            _4708 = global_ddgi_fb[1].w > 0.5f;
                        }
                        else
                        {
                            _4708 = _4702;
                        }
                        bool _4714;
                        if (_4708)
                        {
                            _4714 = global_cache_params.w > 0.5f;
                        }
                        else
                        {
                            _4714 = _4708;
                        }
                        if (_4714)
                        {
                            float ddgiSpanF = max(max(global_ddgi_fb[1].x, global_ddgi_fb[1].y), global_ddgi_fb[1].z) * global_ddgi_fb[0].w;
                            float cExtentF = max(ddgiSpanF * 2.0f, 1.0f);
                            float3 cOrgF = global_ddgi_fb[0].xyz - (cExtentF * 0.25f).xxx;
                            float3 cgpF = (probePos - cOrgF) / (cExtentF / 32.0f).xxx;
                            bool _4751 = all(bool3(cgpF.x >= 0.0f.xxx.x, cgpF.y >= 0.0f.xxx.y, cgpF.z >= 0.0f.xxx.z));
                            bool _4757;
                            if (_4751)
                            {
                                _4757 = all(bool3(cgpF.x < 32.0f.xxx.x, cgpF.y < 32.0f.xxx.y, cgpF.z < 32.0f.xxx.z));
                            }
                            else
                            {
                                _4757 = _4751;
                            }
                            if (_4757)
                            {
                                int3 ctF = clamp(int3(floor(cgpF)), int3(0, 0, 0), int3(31, 31, 31));
                                float4 cL0F = radianceCascadeL0.Load(int4(ctF, 0));
                                bool _4771 = cL0F.w > 0.5f;
                                bool _4778;
                                if (_4771)
                                {
                                    _4778 = any(bool3(cL0F.xyz.x > 0.0f.xxx.x, cL0F.xyz.y > 0.0f.xxx.y, cL0F.xyz.z > 0.0f.xxx.z));
                                }
                                else
                                {
                                    _4778 = _4771;
                                }
                                if (_4778)
                                {
                                    float3 fb_1 = cL0F.xyz * 0.2820949852466583251953125f;
                                    bool _4787 = any(isnan(fb_1));
                                    bool _4794;
                                    if (!_4787)
                                    {
                                        _4794 = any(isinf(fb_1));
                                    }
                                    else
                                    {
                                        _4794 = _4787;
                                    }
                                    if (_4794)
                                    {
                                        fb_1 = 0.0f.xxx;
                                    }
                                    float3 param_77 = rayDir;
                                    float3 cap = (EvaluateSkyBase(param_77) * global_sky_params.xyz) * global_sky_params.w;
                                    radiance = min(fb_1, cap);
                                    cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                                    servedF1a = true;
                                }
                            }
                        }
                        uint _4818;
                        _3600.InterlockedAdd((servedF1a ? 6u : 5u) * 4 + 0, 1u, _4818);
                    }
                }
            }
        }
    }
    bool _4838;
    if (!useWorldFallback)
    {
        bool _4825 = hit.w != 1.0f;
        bool _4831;
        if (_4825)
        {
            _4831 = hit.w != 2.0f;
        }
        else
        {
            _4831 = _4825;
        }
        bool _4837;
        if (_4831)
        {
            _4837 = hit.w != 3.0f;
        }
        else
        {
            _4837 = _4831;
        }
        _4838 = _4837;
    }
    else
    {
        _4838 = useWorldFallback;
    }
    if (_4838)
    {
        bool ddgiOk_1 = false;
        if (global_ddgi_fb[1].w > 0.5f)
        {
            float3 org_1 = global_ddgi_fb[0].xyz;
            float spacing_1 = max(global_ddgi_fb[0].w, 0.00999999977648258209228515625f);
            int3 counts_1 = int3(global_ddgi_fb[1].xyz);
            float3 gp_2 = (probePos - org_1) / spacing_1.xxx;
            float3 gridMax_1 = float3(counts_1 - int3(1, 1, 1));
            bool _4873 = all(bool3(gp_2.x >= 0.0f.xxx.x, gp_2.y >= 0.0f.xxx.y, gp_2.z >= 0.0f.xxx.z));
            bool _4880;
            if (_4873)
            {
                _4880 = all(bool3(gp_2.x <= gridMax_1.x, gp_2.y <= gridMax_1.y, gp_2.z <= gridMax_1.z));
            }
            else
            {
                _4880 = _4873;
            }
            if (_4880)
            {
                int3 b0_1 = clamp(int3(floor(gp_2)), int3(0, 0, 0), counts_1 - int3(1, 1, 1));
                int3 b1 = min((b0_1 + int3(1, 1, 1)), (counts_1 - int3(1, 1, 1)));
                float3 fr = clamp(frac(gp_2), 0.0f.xxx, 1.0f.xxx);
                float3 acc_1 = 0.0f.xxx;
                float wsum_1 = 0.0f;
                float unoccludedWsum = 0.0f;
                float3 _5078;
                float3 _5082;
                float3 _5099;
                float3 _5128;
                float _5194;
                float visibility;
                float _5256;
                float _5261;
                float _5289;
                for (int z = 0; z < 2; z++)
                {
                    for (int y = 0; y < 2; y++)
                    {
                        for (int x = 0; x < 2; x++)
                        {
                            float tw = (lerp(1.0f - fr.x, fr.x, float(x)) * lerp(1.0f - fr.y, fr.y, float(y))) * lerp(1.0f - fr.z, fr.z, float(z));
                            if (tw <= 0.0f)
                            {
                                continue;
                            }
                            int3 pc = int3(lerp(float3(b0_1), float3(b1), float3(int3(x, y, z))));
                            uint pidx_1 = uint(pc.x) + (uint(counts_1.x) * (uint(pc.y) + (uint(counts_1.y) * uint(pc.z))));
                            float3 toProbe = normalize((org_1 + (float3(pc) * spacing_1)) - probePos);
                            float dirW = 0.25f + (0.75f * clamp((dot(toProbe, rayDir) * 0.5f) + 0.5f, 0.0f, 1.0f));
                            float w = tw * dirW;
                            unoccludedWsum += w;
                            if ((global_hint_params.w > 0.5f) || measuredVisibility)
                            {
                                bool _5050;
                                if (measuredVisibility)
                                {
                                    uint _5032;
                                    ddgiOrigins.GetDimensions(_5032);
                                    _5032 = (_5032 - 0) / 16;
                                    bool _5035 = pidx_1 >= uint(int(_5032));
                                    bool _5049;
                                    if (!_5035)
                                    {
                                        uint _5044;
                                        ddgiDepth.GetDimensions(_5044);
                                        _5044 = (_5044 - 0) / 4;
                                        _5049 = pidx_1 >= (uint(int(_5044)) / 128u);
                                    }
                                    else
                                    {
                                        _5049 = _5035;
                                    }
                                    _5050 = _5049;
                                }
                                else
                                {
                                    _5050 = measuredVisibility;
                                }
                                if (_5050)
                                {
                                    continue;
                                }
                                float4 relocated = asfloat(ddgiOrigins.Load4(pidx_1 * 16 + 0));
                                bool _5061 = relocated.w > 0.25f;
                                bool _5068;
                                if (_5061)
                                {
                                    _5068 = !any(isnan(relocated));
                                }
                                else
                                {
                                    _5068 = _5061;
                                }
                                bool _5075;
                                if (_5068)
                                {
                                    _5075 = !any(isinf(relocated));
                                }
                                else
                                {
                                    _5075 = _5068;
                                }
                                bool measuredOrigin = _5075;
                                if (measuredVisibility)
                                {
                                    if (measuredOrigin)
                                    {
                                        _5082 = relocated.xyz;
                                    }
                                    else
                                    {
                                        _5082 = org_1 + (float3(pc) * spacing_1);
                                    }
                                    _5078 = _5082;
                                }
                                else
                                {
                                    if (relocated.w > 0.0f)
                                    {
                                        _5099 = relocated.xyz;
                                    }
                                    else
                                    {
                                        _5099 = org_1 + (float3(pc) * spacing_1);
                                    }
                                    _5078 = _5099;
                                }
                                float3 anchor = _5078;
                                float3 delta_1 = probePos - anchor;
                                float distanceToProbe = length(delta_1);
                                bool _5127;
                                if (measuredVisibility)
                                {
                                    _5127 = !(distanceToProbe > 9.9999997473787516355514526367188e-05f);
                                }
                                else
                                {
                                    _5127 = measuredVisibility;
                                }
                                if (_5127)
                                {
                                    _5128 = probeNormal;
                                }
                                else
                                {
                                    _5128 = delta_1 / max(distanceToProbe, 9.9999997473787516355514526367188e-05f).xxx;
                                }
                                float3 direction = _5128;
                                float2 oct = direction.xy / max(dot(abs(direction), 1.0f.xxx), 9.9999997473787516355514526367188e-05f).xx;
                                if (direction.z < 0.0f)
                                {
                                    bool2 _5160 = bool2(oct.x >= 0.0f.xx.x, oct.y >= 0.0f.xx.y);
                                    oct = (1.0f.xx - abs(oct.yx)) * float2(_5160.x ? 1.0f.xx.x : (-1.0f).xx.x, _5160.y ? 1.0f.xx.y : (-1.0f).xx.y);
                                }
                                uint2 texel = uint2(clamp(((oct * 0.5f) + 0.5f.xx) * 8.0f, 0.0f.xx, 7.0f.xx));
                                uint depthIndex = ((pidx_1 * 128u) + (texel.y * 8u)) + texel.x;
                                float mean = asfloat(ddgiDepth.Load(depthIndex * 4 + 0));
                                float storedVariance = asfloat(ddgiDepth.Load((depthIndex + 64u) * 4 + 0));
                                if (measuredVisibility)
                                {
                                    _5194 = storedVariance;
                                }
                                else
                                {
                                    _5194 = max(storedVariance, 9.9999997473787516355514526367188e-05f);
                                }
                                float variance = _5194;
                                float beyond = max(distanceToProbe - mean, 0.0f);
                                if (measuredVisibility)
                                {
                                    bool _5217;
                                    if (measuredOrigin)
                                    {
                                        _5217 = !isnan(distanceToProbe);
                                    }
                                    else
                                    {
                                        _5217 = measuredOrigin;
                                    }
                                    bool _5223;
                                    if (_5217)
                                    {
                                        _5223 = !isinf(distanceToProbe);
                                    }
                                    else
                                    {
                                        _5223 = _5217;
                                    }
                                    bool _5229;
                                    if (_5223)
                                    {
                                        _5229 = !isnan(mean);
                                    }
                                    else
                                    {
                                        _5229 = _5223;
                                    }
                                    bool _5235;
                                    if (_5229)
                                    {
                                        _5235 = !isinf(mean);
                                    }
                                    else
                                    {
                                        _5235 = _5229;
                                    }
                                    bool _5238 = _5235 && (mean >= 0.0f);
                                    bool _5244;
                                    if (_5238)
                                    {
                                        _5244 = !isnan(variance);
                                    }
                                    else
                                    {
                                        _5244 = _5238;
                                    }
                                    bool _5250;
                                    if (_5244)
                                    {
                                        _5250 = !isinf(variance);
                                    }
                                    else
                                    {
                                        _5250 = _5244;
                                    }
                                    bool known = _5250 && (variance >= 0.0f);
                                    if (known)
                                    {
                                        if (beyond == 0.0f)
                                        {
                                            _5261 = 1.0f;
                                        }
                                        else
                                        {
                                            _5261 = variance / (variance + (beyond * beyond));
                                        }
                                        _5256 = _5261;
                                    }
                                    else
                                    {
                                        _5256 = 0.0f;
                                    }
                                    visibility = _5256;
                                }
                                else
                                {
                                    visibility = clamp((2.0f * variance) / (variance + (beyond * beyond)), 0.20000000298023223876953125f, 1.0f);
                                }
                                if (relocated.w > 0.75f)
                                {
                                    _5289 = 1.0f;
                                }
                                else
                                {
                                    _5289 = (relocated.w > 0.0f) ? 0.5f : 0.0f;
                                }
                                visibility *= _5289;
                                if (auditCardSlot != 4294967295u)
                                {
                                    uint ordinal = uint(((z * 4) + (y * 2)) + x);
                                    _2657.Store4(auditCardSlot * 384 + ordinal * 48 + 13808, asuint(float4(anchor, relocated.w)));
                                    _2657.Store4(auditCardSlot * 384 + ordinal * 48 + 13824, asuint(float4(distanceToProbe, mean, variance, float(pidx_1))));
                                    _2657.Store4(auditCardSlot * 384 + ordinal * 48 + 13840, asuint(float4(w, visibility, 0.0f, float((texel.y * 8u) + texel.x))));
                                }
                                w *= visibility;
                            }
                            float3 sh0v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 0u) * 16 + 0)).xyz;
                            float3 sh1v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 1u) * 16 + 0)).xyz;
                            float3 sh2v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 2u) * 16 + 0)).xyz;
                            float3 sh3v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 3u) * 16 + 0)).xyz;
                            float3 sh4v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 4u) * 16 + 0)).xyz;
                            float3 sh5v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 5u) * 16 + 0)).xyz;
                            float3 sh6v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 6u) * 16 + 0)).xyz;
                            float3 sh7v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 7u) * 16 + 0)).xyz;
                            float3 sh8v = asfloat(ddgiIrr.Load4(((pidx_1 * 9u) + 8u) * 16 + 0)).xyz;
                            float dx = rayDir.x;
                            float dy = rayDir.y;
                            float dz = rayDir.z;
                            float3 irr_2 = ((((((((sh0v * 0.2820949852466583251953125f) + (sh1v * ((-0.48860299587249755859375f) * dy))) + (sh2v * (0.48860299587249755859375f * dz))) + (sh3v * ((-0.48860299587249755859375f) * dx))) + (sh4v * ((1.09254801273345947265625f * dy) * dx))) + (sh5v * (((-1.09254801273345947265625f) * dy) * dz))) + (sh6v * (0.3153919875621795654296875f * (((3.0f * dz) * dz) - 1.0f)))) + (sh7v * (((-1.09254801273345947265625f) * dx) * dz))) + (sh8v * (0.546274006366729736328125f * ((dx * dx) - (dy * dy))));
                            bool _5496;
                            if (measuredVisibility)
                            {
                                bool _5488 = any(isnan(irr_2));
                                bool _5495;
                                if (!_5488)
                                {
                                    _5495 = any(isinf(irr_2));
                                }
                                else
                                {
                                    _5495 = _5488;
                                }
                                _5496 = _5495;
                            }
                            else
                            {
                                _5496 = measuredVisibility;
                            }
                            if (_5496)
                            {
                                continue;
                            }
                            if ((global_hint_params.w > 0.5f) && (auditCardSlot != 4294967295u))
                            {
                                _2657.Store(auditCardSlot * 384 + uint(((z * 4) + (y * 2)) + x) * 48 + 13848, asuint(dot(max(irr_2, 0.0f.xxx), float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f))));
                            }
                            acc_1 += (max(irr_2, 0.0f.xxx) * w);
                            wsum_1 += w;
                        }
                    }
                }
                bool _5542;
                if (measuredVisibility)
                {
                    _5542 = !(wsum_1 > 0.0f);
                }
                else
                {
                    _5542 = measuredVisibility;
                }
                if (_5542)
                {
                    probeRadiance.Store4(global_id * 16 + 0, asuint(0.0f.xxxx));
                    return;
                }
                bool _5549 = wsum_1 > 0.0f;
                bool _5556;
                if (!_5549)
                {
                    _5556 = global_hint_params.w > 0.5f;
                }
                else
                {
                    _5556 = _5549;
                }
                if (_5556)
                {
                    radiance = acc_1 / max(wsum_1, 9.9999997473787516355514526367188e-05f).xxx;
                    if ((global_hint_params.w > 1.5f) || measuredVisibility)
                    {
                        radiance *= clamp(wsum_1 / max(unoccludedWsum, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f);
                    }
                    bool _5580 = any(isnan(radiance));
                    bool _5587;
                    if (!_5580)
                    {
                        _5587 = any(isinf(radiance));
                    }
                    else
                    {
                        _5587 = _5580;
                    }
                    if (_5587)
                    {
                        radiance = 0.0f.xxx;
                    }
                    radiance = min(radiance, 64.0f.xxx);
                    cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                    ddgiOk_1 = true;
                }
            }
        }
        if ((!ddgiOk_1) && measuredVisibility)
        {
            probeRadiance.Store4(global_id * 16 + 0, asuint(0.0f.xxxx));
            return;
        }
        if (!ddgiOk_1)
        {
            float skyWeight_1 = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight_1 > 0.0f)
            {
                float3 param_78 = rayDir;
                radiance = EvaluateSkyBase(param_78) * global_sky_params.xyz;
                cosTheta = skyWeight_1;
            }
            else
            {
                bool servedF1a_1 = false;
                bool _5630 = global_sky_params.w > 0.0f;
                bool _5636;
                if (_5630)
                {
                    _5636 = global_ddgi_fb[1].w > 0.5f;
                }
                else
                {
                    _5636 = _5630;
                }
                bool _5642;
                if (_5636)
                {
                    _5642 = global_cache_params.w > 0.5f;
                }
                else
                {
                    _5642 = _5636;
                }
                if (_5642)
                {
                    float ddgiSpanF_1 = max(max(global_ddgi_fb[1].x, global_ddgi_fb[1].y), global_ddgi_fb[1].z) * global_ddgi_fb[0].w;
                    float cExtentF_1 = max(ddgiSpanF_1 * 2.0f, 1.0f);
                    float3 cOrgF_1 = global_ddgi_fb[0].xyz - (cExtentF_1 * 0.25f).xxx;
                    float3 cgpF_1 = (probePos - cOrgF_1) / (cExtentF_1 / 32.0f).xxx;
                    bool _5679 = all(bool3(cgpF_1.x >= 0.0f.xxx.x, cgpF_1.y >= 0.0f.xxx.y, cgpF_1.z >= 0.0f.xxx.z));
                    bool _5685;
                    if (_5679)
                    {
                        _5685 = all(bool3(cgpF_1.x < 32.0f.xxx.x, cgpF_1.y < 32.0f.xxx.y, cgpF_1.z < 32.0f.xxx.z));
                    }
                    else
                    {
                        _5685 = _5679;
                    }
                    if (_5685)
                    {
                        int3 ctF_1 = clamp(int3(floor(cgpF_1)), int3(0, 0, 0), int3(31, 31, 31));
                        float4 cL0F_1 = radianceCascadeL0.Load(int4(ctF_1, 0));
                        bool _5699 = cL0F_1.w > 0.5f;
                        bool _5706;
                        if (_5699)
                        {
                            _5706 = any(bool3(cL0F_1.xyz.x > 0.0f.xxx.x, cL0F_1.xyz.y > 0.0f.xxx.y, cL0F_1.xyz.z > 0.0f.xxx.z));
                        }
                        else
                        {
                            _5706 = _5699;
                        }
                        if (_5706)
                        {
                            float3 fb_2 = cL0F_1.xyz * 0.2820949852466583251953125f;
                            bool _5715 = any(isnan(fb_2));
                            bool _5722;
                            if (!_5715)
                            {
                                _5722 = any(isinf(fb_2));
                            }
                            else
                            {
                                _5722 = _5715;
                            }
                            if (_5722)
                            {
                                fb_2 = 0.0f.xxx;
                            }
                            float3 param_79 = rayDir;
                            float3 cap_1 = (EvaluateSkyBase(param_79) * global_sky_params.xyz) * global_sky_params.w;
                            radiance = min(fb_2, cap_1);
                            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                            servedF1a_1 = true;
                        }
                    }
                }
                uint _5746;
                _3600.InterlockedAdd((servedF1a_1 ? 6u : 5u) * 4 + 0, 1u, _5746);
            }
        }
        uint param_80 = probeIdx;
        uint param_81 = ddgiOk_1 ? 4u : 5u;
        float3 param_82 = radiance;
        float param_83 = cosTheta;
        spgiStatRecord(param_80, param_81, param_82, param_83);
        if (hit.w == 2.0f)
        {
            uint param_84 = probeIdx;
            uint param_85 = 6u;
            float3 param_86 = radiance;
            float param_87 = hit.x;
            spgiStatRecord(param_84, param_85, param_86, param_87);
        }
    }
    if (auditCardSlot != 4294967295u)
    {
        uint slot = auditCardSlot;
        _2657.Store4(slot * 64 + 1904, asuint(float4(origin + (rayDir * max(hit.x, 0.0f)), hit.x)));
        _2657.Store4(slot * 64 + 1920, asuint(float4(probePos, float(probeIdx))));
        float _5805;
        if (hit.w == 2.0f)
        {
            _5805 = float(scRejectReason);
        }
        else
        {
            float _5814;
            if (hit.w == 1.0f)
            {
                _5814 = -1.0f;
            }
            else
            {
                _5814 = (hit.w == 3.0f) ? (-3.0f) : (-4.0f);
            }
            _5805 = _5814;
        }
        float reason = _5805;
        _2657.Store4(slot * 64 + 1936, asuint(float4(rayDir, reason)));
        float _5844;
        if (hit.w == 2.0f)
        {
            _5844 = hit.z;
        }
        else
        {
            _5844 = -1.0f;
        }
        _2657.Store4(slot * 64 + 1952, asuint(float4(float(auditEvalCount), float(auditBounds), float(auditOrient), _5844)));
    }
    bool geoHit = statBranch <= 1u;
    uint param_88 = probeIdx;
    uint param_89 = statBranch;
    float3 param_90 = radiance;
    float param_91 = cosTheta;
    spgiStatRecord(param_88, param_89, param_90, param_91);
    bool _5868 = global_diag_params.x > 3.5f;
    bool _5875;
    if (_5868)
    {
        _5875 = global_diag_params.x < 4.5f;
    }
    else
    {
        _5875 = _5868;
    }
    if (_5875)
    {
        if (statBranch <= 1u)
        {
            radiance = 0.0f.xxx;
        }
    }
    else
    {
        bool _5885 = global_diag_params.x > 4.5f;
        bool _5891;
        if (_5885)
        {
            _5891 = global_diag_params.x < 5.5f;
        }
        else
        {
            _5891 = _5885;
        }
        if (_5891)
        {
            if (statBranch >= 2u)
            {
                radiance = 0.0f.xxx;
            }
        }
    }
    if (global_diag_params.x > 0.5f)
    {
        float4 clip = mul(float4(probePos, 1.0f), global_view_projection);
        if (clip.w > 0.0f)
        {
            float2 uv_1 = float2(((clip.x / clip.w) * 0.5f) + 0.5f, 0.5f - ((clip.y / clip.w) * 0.5f));
            uint category = (statBranch <= 1u) ? 2u : 3u;
            bool _5944;
            if (useWorldFallback)
            {
                _5944 = hit.w == 2.0f;
            }
            else
            {
                _5944 = useWorldFallback;
            }
            if (_5944)
            {
                category = (hit.x <= (global_sdf_voxel_sizes[0].x * 0.5f)) ? 0u : 1u;
            }
            float lum = clamp(dot(radiance, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)), 0.0f, 64.0f);
            for (uint r = 0u; r < 6u; r++)
            {
                bool _5996 = all(bool2(uv_1.x >= _5988[r].xy.x, uv_1.y >= _5988[r].xy.y));
                bool _6007;
                if (_5996)
                {
                    _6007 = all(bool2(uv_1.x < _5988[r].zw.x, uv_1.y < _5988[r].zw.y));
                }
                else
                {
                    _6007 = _5996;
                }
                if (_6007)
                {
                    uint _6013;
                    _2657.InterlockedAdd(r * 64 + category * 16 + 13424, 1u, _6013);
                    uint _6020;
                    _2657.InterlockedAdd(r * 64 + category * 16 + 13428, uint(lum * 64.0f), _6020);
                    uint _6030;
                    _2657.InterlockedAdd(r * 64 + category * 16 + 13432, uint((lum * max(cosTheta, 0.0f)) * 64.0f), _6030);
                    uint _6039;
                    _2657.InterlockedAdd(r * 64 + category * 16 + 13436, uint(clamp(cosTheta, 0.0f, 1.0f) * 256.0f), _6039);
                }
            }
        }
    }
    float _6045;
    if (geoHit)
    {
        _6045 = cosTheta * invNq;
    }
    else
    {
        _6045 = (-cosTheta) * invNq;
    }
    probeRadiance.Store4(global_id * 16 + 0, asuint(float4(radiance, _6045)));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
