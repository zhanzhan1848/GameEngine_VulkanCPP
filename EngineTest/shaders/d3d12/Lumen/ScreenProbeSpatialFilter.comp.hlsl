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

ByteAddressBuffer geometryTop : register(t5, space0);
ByteAddressBuffer geometryInstances : register(t8, space0);
ByteAddressBuffer geometryBottom : register(t6, space0);
ByteAddressBuffer geometryTriangles : register(t7, space0);
cbuffer SpatialConstantsBlock : register(b3, space0)
{
    uint params_totalProbes : packoffset(c0);
    uint params_gridW : packoffset(c0.y);
    float params_sigma : packoffset(c0.z);
    uint params_pad : packoffset(c0.w);
};

ByteAddressBuffer probeNormal : register(t4, space0);
ByteAddressBuffer probePos : register(t2, space0);
RWByteAddressBuffer outputSH : register(u1, space0);
ByteAddressBuffer inputSH : register(t0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

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
            bool _155 = o[k] < n.lo[k];
            bool _167;
            if (!_155)
            {
                _167 = o[k] > n.hi[k];
            }
            else
            {
                _167 = _155;
            }
            if (_167)
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
    precise float _69 = c * d;
    float cd = _69;
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
    bool _279;
    if (false)
    {
        _279 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _279 = false;
    }
    bool _286;
    if (_279)
    {
        _286 = !any(isnan(d));
    }
    else
    {
        _286 = _279;
    }
    bool _293;
    if (_286)
    {
        _293 = !any(isinf(d));
    }
    else
    {
        _293 = _286;
    }
    bool topNonzero = _293;
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
    float3 _851;
    while (tp != 0u)
    {
        uint _311 = tp;
        uint _312 = _311 - uint(1);
        tp = _312;
        uint at = ts[_312];
        uint _321;
        geometryTop.GetDimensions(_321);
        _321 = (_321 - 0) / 48;
        if (at >= uint(int(_321)))
        {
            return -1;
        }
        GeometryNode _333;
        _333.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _333.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _333.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _333.lo;
        n.hi = _333.hi;
        n.links = _333.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _357 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _357;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _369 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _369;
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
            uint _386 = tp;
            tp = _386 + uint(1);
            ts[_386] = n.links.x;
            uint _391 = tp;
            tp = _391 + uint(1);
            ts[_391] = n.links.y;
            continue;
        }
        uint _404;
        geometryInstances.GetDimensions(_404);
        _404 = (_404 - 0) / 64;
        if (n.links.z >= uint(int(_404)))
        {
            return -1;
        }
        GeometryInstance _418;
        _418.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64 + 0));
        _418.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _418.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _418.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _418.inverse0;
        inst.inverse1 = _418.inverse1;
        inst.inverse2 = _418.inverse2;
        inst.identity = _418.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        bool _474 = any(isnan(lo));
        bool _481;
        if (!_474)
        {
            _481 = any(isinf(lo));
        }
        else
        {
            _481 = _474;
        }
        bool _488;
        if (!_481)
        {
            _488 = any(isnan(ld));
        }
        else
        {
            _488 = _481;
        }
        bool _495;
        if (!_488)
        {
            _495 = any(isinf(ld));
        }
        else
        {
            _495 = _488;
        }
        if (_495)
        {
            return -1;
        }
        bool _505;
        if (false)
        {
            _505 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _505 = false;
        }
        bool bottomNonzero = _505;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _518 = bp;
            uint _519 = _518 - uint(1);
            bp = _519;
            at = bs[_519];
            uint _527;
            geometryBottom.GetDimensions(_527);
            _527 = (_527 - 0) / 48;
            if (at >= uint(int(_527)))
            {
                return -1;
            }
            GeometryNode _537;
            _537.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _537.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _537.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _537.lo;
            b.hi = _537.hi;
            b.links = _537.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _558 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _558;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _570 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _570;
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
                uint _587 = bp;
                bp = _587 + uint(1);
                bs[_587] = b.links.x;
                uint _592 = bp;
                bp = _592 + uint(1);
                bs[_592] = b.links.y;
                continue;
            }
            uint _605;
            geometryTriangles.GetDimensions(_605);
            _605 = (_605 - 0) / 48;
            bool _608 = b.links.z > uint(int(_605));
            bool _621;
            if (!_608)
            {
                uint _614;
                geometryTriangles.GetDimensions(_614);
                _614 = (_614 - 0) / 48;
                _621 = b.links.w > (uint(int(_614)) - b.links.z);
            }
            else
            {
                _621 = _608;
            }
            if (_621)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _644;
                _644.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 0));
                _644.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _644.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _644.a;
                tri.e1 = _644.e1;
                tri.e2 = _644.e2;
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
                bool _730 = isnan(t);
                bool _736;
                if (!_730)
                {
                    _736 = isinf(t);
                }
                else
                {
                    _736 = _730;
                }
                bool _742;
                if (!_736)
                {
                    _742 = isnan(bary);
                }
                else
                {
                    _742 = _736;
                }
                bool _748;
                if (!_742)
                {
                    _748 = isinf(bary);
                }
                else
                {
                    _748 = _742;
                }
                bool _754;
                if (!_748)
                {
                    _754 = isnan(u);
                }
                else
                {
                    _754 = _748;
                }
                bool _760;
                if (!_754)
                {
                    _760 = isinf(u);
                }
                else
                {
                    _760 = _754;
                }
                if (_760)
                {
                    return -1;
                }
                bool _765 = bary < 0.0f;
                bool _773;
                if (!_765)
                {
                    _773 = (u + bary) > 1.0f;
                }
                else
                {
                    _773 = _765;
                }
                bool _777 = _773 || (t < nearT);
                bool _784;
                if (!_777)
                {
                    _784 = t > bestT;
                }
                else
                {
                    _784 = _777;
                }
                if (_784)
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _798;
                if (found)
                {
                    _798 = t == bestT;
                }
                else
                {
                    _798 = found;
                }
                bool _819;
                if (_798)
                {
                    bool _804 = inst.identity.y > owner;
                    bool _818;
                    if (!_804)
                    {
                        bool _811 = inst.identity.y == owner;
                        bool _817;
                        if (_811)
                        {
                            _817 = primitive >= bestPrimitive;
                        }
                        else
                        {
                            _817 = _811;
                        }
                        _818 = _817;
                    }
                    else
                    {
                        _818 = _804;
                    }
                    _819 = _818;
                }
                else
                {
                    _819 = _798;
                }
                if (_819)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
                bool _833 = all(bool4(inst.inverse0.x == float4(1.0f, 0.0f, 0.0f, 0.0f).x, inst.inverse0.y == float4(1.0f, 0.0f, 0.0f, 0.0f).y, inst.inverse0.z == float4(1.0f, 0.0f, 0.0f, 0.0f).z, inst.inverse0.w == float4(1.0f, 0.0f, 0.0f, 0.0f).w));
                bool _841;
                if (_833)
                {
                    _841 = all(bool4(inst.inverse1.x == float4(0.0f, 1.0f, 0.0f, 0.0f).x, inst.inverse1.y == float4(0.0f, 1.0f, 0.0f, 0.0f).y, inst.inverse1.z == float4(0.0f, 1.0f, 0.0f, 0.0f).z, inst.inverse1.w == float4(0.0f, 1.0f, 0.0f, 0.0f).w));
                }
                else
                {
                    _841 = _833;
                }
                bool _849;
                if (_841)
                {
                    _849 = all(bool4(inst.inverse2.x == float4(0.0f, 0.0f, 1.0f, 0.0f).x, inst.inverse2.y == float4(0.0f, 0.0f, 1.0f, 0.0f).y, inst.inverse2.z == float4(0.0f, 0.0f, 1.0f, 0.0f).z, inst.inverse2.w == float4(0.0f, 0.0f, 1.0f, 0.0f).w));
                }
                else
                {
                    _849 = _841;
                }
                bool identity = _849;
                if (identity)
                {
                    _851 = normalize(normal);
                }
                else
                {
                    _851 = 0.0f.xxx;
                }
                firstN = _851;
                firstA = tri.a.xyz;
                firstE1 = tri.e1.xyz;
                firstE2 = tri.e2.xyz;
            }
        }
    }
    return int(found);
}

bool measuredSpatialConnection(uint center, uint neighbor, float3 p, float3 q)
{
    if ((params_pad & 2u) == 0u)
    {
        return false;
    }
    float3 n = asfloat(probeNormal.Load4(center * 16 + 0)).xyz;
    float3 m = asfloat(probeNormal.Load4(neighbor * 16 + 0)).xyz;
    bool _901 = any(isnan(n));
    bool _908;
    if (!_901)
    {
        _908 = any(isinf(n));
    }
    else
    {
        _908 = _901;
    }
    bool _915;
    if (!_908)
    {
        _915 = any(isnan(m));
    }
    else
    {
        _915 = _908;
    }
    bool _922;
    if (!_915)
    {
        _922 = any(isinf(m));
    }
    else
    {
        _922 = _915;
    }
    bool _931;
    if (!_922)
    {
        _931 = dot(n, n) < 0.25f;
    }
    else
    {
        _931 = _922;
    }
    bool _939;
    if (!_931)
    {
        _939 = dot(m, m) < 0.25f;
    }
    else
    {
        _939 = _931;
    }
    if (_939)
    {
        return false;
    }
    n = normalize(n);
    m = normalize(m);
    if (dot(n, m) < 0.5f)
    {
        return false;
    }
    float3 a = p + (n * 0.00999999977648258209228515625f);
    float3 b = q + (m * 0.00999999977648258209228515625f);
    float3 delta = b - a;
    float _distance = length(delta);
    bool _974 = isnan(_distance);
    bool _980;
    if (!_974)
    {
        _980 = isinf(_distance);
    }
    else
    {
        _980 = _974;
    }
    if (_980 || (_distance <= 0.00200000009499490261077880859375f))
    {
        return false;
    }
    float3 param = a;
    float3 param_1 = delta / _distance.xxx;
    float param_2 = 0.001000000047497451305389404296875f;
    float param_3 = _distance - 0.001000000047497451305389404296875f;
    float param_4;
    uint param_5;
    uint param_6;
    float3 param_7;
    float3 param_8;
    float3 param_9;
    float3 param_10;
    int _1016 = geometryFirstHit(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9, param_10);
    float t = param_4;
    uint owner = param_5;
    uint primitive = param_6;
    float3 gn = param_7;
    float3 ga = param_8;
    float3 ge1 = param_9;
    float3 ge2 = param_10;
    int outcome = _1016;
    return outcome == 0;
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    if (global_id >= params_totalProbes)
    {
        return;
    }
    uint gx = global_id % params_gridW;
    uint gy = global_id / params_gridW;
    float4 centerPosData = asfloat(probePos.Load4(global_id * 16 + 0));
    bool _1062 = centerPosData.w <= 0.0f;
    bool _1083;
    if (!_1062)
    {
        bool _1069 = (params_pad & 1u) != 0u;
        bool _1082;
        if (_1069)
        {
            bool _1074 = any(isnan(centerPosData));
            bool _1081;
            if (!_1074)
            {
                _1081 = any(isinf(centerPosData));
            }
            else
            {
                _1081 = _1074;
            }
            _1082 = _1081;
        }
        else
        {
            _1082 = _1069;
        }
        _1083 = _1082;
    }
    else
    {
        _1083 = _1062;
    }
    if (_1083)
    {
        uint base = global_id * 4u;
        outputSH.Store4((base + 0u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 1u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 2u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 3u) * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    uint centerBase = global_id * 4u;
    float3 centerSH[4];
    for (int b = 0; b < 4; b++)
    {
        centerSH[b] = asfloat(inputSH.Load4((centerBase + uint(b)) * 16 + 0)).xyz;
    }
    bool finiteCenter = true;
    if ((params_pad & 1u) != 0u)
    {
        for (int b_1 = 0; b_1 < 4; b_1++)
        {
            bool _1165;
            if (finiteCenter)
            {
                _1165 = !any(isnan(asfloat(inputSH.Load4((centerBase + uint(b_1)) * 16 + 0))));
            }
            else
            {
                _1165 = finiteCenter;
            }
            bool _1177;
            if (_1165)
            {
                _1177 = !any(isinf(asfloat(inputSH.Load4((centerBase + uint(b_1)) * 16 + 0))));
            }
            else
            {
                _1177 = _1165;
            }
            finiteCenter = _1177;
        }
    }
    if ((asfloat(inputSH.Load((centerBase + 0u) * 16 + 12)) <= 0.0f) || (!finiteCenter))
    {
        outputSH.Store4((centerBase + 0u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((centerBase + 1u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((centerBase + 2u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((centerBase + 3u) * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    float3 centerPos = centerPosData.xyz;
    float invTwoSigmaSq = 1.0f / ((2.0f * params_sigma) * params_sigma);
    int2 offsets[4];
    offsets[0] = int2(-1, 0);
    offsets[1] = int2(1, 0);
    offsets[2] = int2(0, -1);
    offsets[3] = int2(0, 1);
    float3 _1242[4] = { centerSH[0] * 2.0f, centerSH[1] * 2.0f, centerSH[2] * 2.0f, centerSH[3] * 2.0f };
    float3 filteredSH[4] = _1242;
    float totalWeight = 2.0f;
    float centerConf = asfloat(inputSH.Load((centerBase + 0u) * 16 + 12));
    float filteredConf = centerConf * 2.0f;
    for (int n = 0; n < 4; n++)
    {
        int nx = int(gx) + offsets[n].x;
        int ny = int(gy) + offsets[n].y;
        bool _1275 = nx < 0;
        bool _1284;
        if (!_1275)
        {
            _1284 = nx >= int(params_gridW);
        }
        else
        {
            _1284 = _1275;
        }
        if (_1284 || (ny < 0))
        {
            continue;
        }
        if (((uint(ny) * params_gridW) + uint(nx)) >= params_totalProbes)
        {
            continue;
        }
        uint nidx = (uint(ny) * params_gridW) + uint(nx);
        float4 neighborPosData = asfloat(probePos.Load4(nidx * 16 + 0));
        if (neighborPosData.w <= 0.0f)
        {
            continue;
        }
        uint neighborBase = nidx * 4u;
        if (asfloat(inputSH.Load((neighborBase + 0u) * 16 + 12)) <= 0.0f)
        {
            continue;
        }
        if ((params_pad & 1u) != 0u)
        {
            bool _1345 = !any(isnan(neighborPosData));
            bool _1352;
            if (_1345)
            {
                _1352 = !any(isinf(neighborPosData));
            }
            else
            {
                _1352 = _1345;
            }
            bool finiteNeighbor = _1352;
            for (int b_2 = 0; b_2 < 4; b_2++)
            {
                bool _1373;
                if (finiteNeighbor)
                {
                    _1373 = !any(isnan(asfloat(inputSH.Load4((neighborBase + uint(b_2)) * 16 + 0))));
                }
                else
                {
                    _1373 = finiteNeighbor;
                }
                bool _1385;
                if (_1373)
                {
                    _1385 = !any(isinf(asfloat(inputSH.Load4((neighborBase + uint(b_2)) * 16 + 0))));
                }
                else
                {
                    _1385 = _1373;
                }
                finiteNeighbor = _1385;
            }
            if (!finiteNeighbor)
            {
                continue;
            }
        }
        bool _1396 = (params_pad & 1u) != 0u;
        bool _1410;
        if (_1396)
        {
            uint param = global_id;
            uint param_1 = nidx;
            float3 param_2 = centerPos;
            float3 param_3 = neighborPosData.xyz;
            _1410 = !measuredSpatialConnection(param, param_1, param_2, param_3);
        }
        else
        {
            _1410 = _1396;
        }
        if (_1410)
        {
            continue;
        }
        float3 delta = centerPos - neighborPosData.xyz;
        float dist2 = dot(delta, delta);
        float w = exp((-dist2) * invTwoSigmaSq);
        for (int b_3 = 0; b_3 < 4; b_3++)
        {
            filteredSH[b_3] += (asfloat(inputSH.Load4((neighborBase + uint(b_3)) * 16 + 0)).xyz * w);
        }
        filteredConf += (asfloat(inputSH.Load((neighborBase + 0u) * 16 + 12)) * w);
        totalWeight += w;
    }
    float invWeight = 1.0f / totalWeight;
    uint outBase = global_id * 4u;
    outputSH.Store4((outBase + 0u) * 16 + 0, asuint(float4(filteredSH[0] * invWeight, filteredConf * invWeight)));
    outputSH.Store4((outBase + 1u) * 16 + 0, asuint(float4(filteredSH[1] * invWeight, 1.0f)));
    outputSH.Store4((outBase + 2u) * 16 + 0, asuint(float4(filteredSH[2] * invWeight, 1.0f)));
    outputSH.Store4((outBase + 3u) * 16 + 0, asuint(float4(filteredSH[3] * invWeight, 1.0f)));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
