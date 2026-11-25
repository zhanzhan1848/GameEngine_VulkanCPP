// Vulkan reference: EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp (main).
#include "../Common/TextureDimensions.hlsli"

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

struct SurfaceCacheCardLookup
{
    float4 aabb_min;
    float4 aabb_max;
    uint card_start;
    uint card_count;
    uint _pad0;
    uint _pad1;
};

ByteAddressBuffer geometryTop : register(t13, space0);
ByteAddressBuffer geometryInstances : register(t16, space0);
ByteAddressBuffer geometryBottom : register(t14, space0);
ByteAddressBuffer geometryTriangles : register(t15, space0);
ByteAddressBuffer query : register(t0, space0);
RWByteAddressBuffer result : register(u1, space0);
ByteAddressBuffer lookup : register(t3, space0);
ByteAddressBuffer cards : register(t2, space0);
Texture2D<float4> depth_atlas : register(t4, space0);
Texture2D<float4> lighting_atlas : register(t5, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
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
            if (((o[k] < n.lo[k]) || (o[k] > n.hi[k])))
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
    precise float _902 = c * d;
    float cd = _902;
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

int geometryFirstHit(float3 o, float3 d, float nearT, float farT, inout float bestT, inout uint owner, inout uint bestPrimitive)
{
    bool _1100;
    if (false)
    {
        _1100 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _1100 = false;
    }
    bool _1107;
    if (_1100)
    {
        _1107 = !any(isnan(d));
    }
    else
    {
        _1107 = _1100;
    }
    bool _1114;
    if (_1107)
    {
        _1114 = !any(isinf(d));
    }
    else
    {
        _1114 = _1107;
    }
    bool topNonzero = _1114;
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
    while (tp != 0u)
    {
        uint _1132 = tp;
        uint _1133 = _1132 - 1u;
        tp = _1133;
        uint at = ts[_1133];
        uint _1142;
        geometryTop.GetDimensions(_1142);
        _1142 = (_1142 - 0) / 48;
        if (at >= uint(int(_1142)))
        {
            return -1;
        }
        GeometryNode _1154;
        _1154.lo = asfloat(geometryTop.Load4(at * 48));
        _1154.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _1154.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _1154.lo;
        n.hi = _1154.hi;
        n.links = _1154.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _1176 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _1176;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _1188 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _1188;
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
            uint _1205 = tp;
            tp = _1205 + 1u;
            ts[_1205] = n.links.x;
            uint _1210 = tp;
            tp = _1210 + 1u;
            ts[_1210] = n.links.y;
            continue;
        }
        uint _1223;
        geometryInstances.GetDimensions(_1223);
        _1223 = (_1223 - 0) / 64;
        if (n.links.z >= uint(int(_1223)))
        {
            return -1;
        }
        GeometryInstance _1237;
        _1237.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64));
        _1237.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _1237.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _1237.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _1237.inverse0;
        inst.inverse1 = _1237.inverse1;
        inst.inverse2 = _1237.inverse2;
        inst.identity = _1237.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        if (((((any(isnan(lo))) || (any(isinf(lo)))) || (any(isnan(ld)))) || (any(isinf(ld)))))
        {
            return -1;
        }
        bool _1322;
        if (false)
        {
            _1322 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _1322 = false;
        }
        bool bottomNonzero = _1322;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _1335 = bp;
            uint _1336 = _1335 - 1u;
            bp = _1336;
            at = bs[_1336];
            uint _1344;
            geometryBottom.GetDimensions(_1344);
            _1344 = (_1344 - 0) / 48;
            if (at >= uint(int(_1344)))
            {
                return -1;
            }
            GeometryNode _1354;
            _1354.lo = asfloat(geometryBottom.Load4(at * 48));
            _1354.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1354.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _1354.lo;
            b.hi = _1354.hi;
            b.links = _1354.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _1375 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _1375;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _1387 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _1387;
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
                uint _1404 = bp;
                bp = _1404 + 1u;
                bs[_1404] = b.links.x;
                uint _1409 = bp;
                bp = _1409 + 1u;
                bs[_1409] = b.links.y;
                continue;
            }
            uint _1422;
            geometryTriangles.GetDimensions(_1422);
            _1422 = (_1422 - 0) / 48;
            bool _1425 = b.links.z > uint(int(_1422));
            bool _1438;
            if (!_1425)
            {
                uint _1431;
                geometryTriangles.GetDimensions(_1431);
                _1431 = (_1431 - 0) / 48;
                _1438 = b.links.w > (uint(int(_1431)) - b.links.z);
            }
            else
            {
                _1438 = _1425;
            }
            if (_1438)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _1461;
                _1461.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48));
                _1461.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _1461.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _1461.a;
                tri.e1 = _1461.e1;
                tri.e2 = _1461.e2;
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
                if (((((((isnan(t)) || (isinf(t))) || (isnan(bary))) || (isinf(bary))) || (isnan(u))) || (isinf(u))))
                {
                    return -1;
                }
                bool _1590 = (bary < 0.0f) || ((u + bary) > 1.0f);
                if (((_1590 || (t < nearT)) || (t > bestT)))
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _1615;
                if (found)
                {
                    _1615 = t == bestT;
                }
                else
                {
                    _1615 = found;
                }
                bool _1636;
                if (_1615)
                {
                    bool _1621 = inst.identity.y > owner;
                    bool _1635;
                    if (!_1621)
                    {
                        bool _1634 = (inst.identity.y == owner) && (primitive >= bestPrimitive);
                        _1635 = _1634;
                    }
                    else
                    {
                        _1635 = _1621;
                    }
                    _1636 = _1635;
                }
                else
                {
                    _1636 = _1615;
                }
                if (_1636)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
            }
        }
    }
    return int(found);
}

int geometryFirstHit(float3 o, float3 d, float nearT, float farT, inout float bestT, inout uint owner, inout uint bestPrimitive, out float3 firstN, out float3 firstA, out float3 firstE1, out float3 firstE2)
{
    bool _1656;
    if (false)
    {
        _1656 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _1656 = false;
    }
    bool _1663;
    if (_1656)
    {
        _1663 = !any(isnan(d));
    }
    else
    {
        _1663 = _1656;
    }
    bool _1670;
    if (_1663)
    {
        _1670 = !any(isinf(d));
    }
    else
    {
        _1670 = _1663;
    }
    bool topNonzero = _1670;
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
        uint _1684 = topVisited;
        topVisited = _1684 + 1u;
        uint _1686;
        geometryTop.GetDimensions(_1686);
        _1686 = (_1686 - 0) / 48;
        if (_1684 >= uint(int(_1686)))
        {
            return -1;
        }
        uint _1694 = tp;
        uint _1695 = _1694 - 1u;
        tp = _1695;
        uint at = ts[_1695];
        uint _1699;
        geometryTop.GetDimensions(_1699);
        _1699 = (_1699 - 0) / 48;
        if (at >= uint(int(_1699)))
        {
            return -1;
        }
        GeometryNode _1709;
        _1709.lo = asfloat(geometryTop.Load4(at * 48));
        _1709.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _1709.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _1709.lo;
        n.hi = _1709.hi;
        n.links = _1709.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _1730 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _1730;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _1742 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _1742;
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
            uint _1759 = tp;
            tp = _1759 + 1u;
            ts[_1759] = n.links.x;
            uint _1764 = tp;
            tp = _1764 + 1u;
            ts[_1764] = n.links.y;
            continue;
        }
        uint _1772;
        geometryInstances.GetDimensions(_1772);
        _1772 = (_1772 - 0) / 64;
        if (n.links.z >= uint(int(_1772)))
        {
            return -1;
        }
        GeometryInstance _1783;
        _1783.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64));
        _1783.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _1783.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _1783.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _1783.inverse0;
        inst.inverse1 = _1783.inverse1;
        inst.inverse2 = _1783.inverse2;
        inst.identity = _1783.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        if (((((any(isnan(lo))) || (any(isinf(lo)))) || (any(isnan(ld)))) || (any(isinf(ld)))))
        {
            return -1;
        }
        bool _1868;
        if (false)
        {
            _1868 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _1868 = false;
        }
        bool bottomNonzero = _1868;
        uint bottomVisited = 0u;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _1882 = bottomVisited;
            bottomVisited = _1882 + 1u;
            uint _1884;
            geometryBottom.GetDimensions(_1884);
            _1884 = (_1884 - 0) / 48;
            if (_1882 >= uint(int(_1884)))
            {
                return -1;
            }
            uint _1891 = bp;
            uint _1892 = _1891 - 1u;
            bp = _1892;
            at = bs[_1892];
            uint _1896;
            geometryBottom.GetDimensions(_1896);
            _1896 = (_1896 - 0) / 48;
            if (at >= uint(int(_1896)))
            {
                return -1;
            }
            GeometryNode _1906;
            _1906.lo = asfloat(geometryBottom.Load4(at * 48));
            _1906.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1906.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _1906.lo;
            b.hi = _1906.hi;
            b.links = _1906.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _1927 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _1927;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _1939 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _1939;
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
                uint _1956 = bp;
                bp = _1956 + 1u;
                bs[_1956] = b.links.x;
                uint _1961 = bp;
                bp = _1961 + 1u;
                bs[_1961] = b.links.y;
                continue;
            }
            uint _1969;
            geometryTriangles.GetDimensions(_1969);
            _1969 = (_1969 - 0) / 48;
            bool _1972 = b.links.z > uint(int(_1969));
            bool _1985;
            if (!_1972)
            {
                uint _1978;
                geometryTriangles.GetDimensions(_1978);
                _1978 = (_1978 - 0) / 48;
                _1985 = b.links.w > (uint(int(_1978)) - b.links.z);
            }
            else
            {
                _1985 = _1972;
            }
            if (_1985)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _2005;
                _2005.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48));
                _2005.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _2005.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _2005.a;
                tri.e1 = _2005.e1;
                tri.e2 = _2005.e2;
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
                if (((((((isnan(t)) || (isinf(t))) || (isnan(bary))) || (isinf(bary))) || (isnan(u))) || (isinf(u))))
                {
                    return -1;
                }
                bool _2134 = (bary < 0.0f) || ((u + bary) > 1.0f);
                if (((_2134 || (t < nearT)) || (t > bestT)))
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _2159;
                if (found)
                {
                    _2159 = t == bestT;
                }
                else
                {
                    _2159 = found;
                }
                bool _2180;
                if (_2159)
                {
                    bool _2165 = inst.identity.y > owner;
                    bool _2179;
                    if (!_2165)
                    {
                        bool _2178 = (inst.identity.y == owner) && (primitive >= bestPrimitive);
                        _2179 = _2178;
                    }
                    else
                    {
                        _2179 = _2165;
                    }
                    _2180 = _2179;
                }
                else
                {
                    _2180 = _2159;
                }
                if (_2180)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
                float3 worldN = float3(dot(float3(inst.inverse0.x, inst.inverse1.x, inst.inverse2.x), normal), dot(float3(inst.inverse0.y, inst.inverse1.y, inst.inverse2.y), normal), dot(float3(inst.inverse0.z, inst.inverse1.z, inst.inverse2.z), normal));
                float normalScale = max(abs(worldN.x), max(abs(worldN.y), abs(worldN.z)));
                bool _2238 = (any(isnan(worldN))) || (any(isinf(worldN)));
                if (_2238 || (normalScale <= 0.0f))
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

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _517;
    if (axis == 0u)
    {
        _517 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _525;
        if (axis == 1u)
        {
            _525 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _525 = float3(0.0f, 0.0f, s);
        }
        _517 = _525;
    }
    return _517;
}

bool worldToCardUVMarginPad(float3 world_pos, SurfaceCacheCard card, inout float2 out_uv, float margin_uv, float pad_m)
{
    float3 local = world_pos - card.center.xyz;
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
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
        float _350;
        if (axis == 0u)
        {
            _350 = card.extent.y;
        }
        else
        {
            float _358;
            if (axis == 1u)
            {
                _358 = card.extent.x;
            }
            else
            {
                _358 = card.extent.x;
            }
            _350 = _358;
        }
        float eu = _350;
        float _371;
        if (axis == 0u)
        {
            _371 = card.extent.z;
        }
        else
        {
            float _379;
            if (axis == 1u)
            {
                _379 = card.extent.z;
            }
            else
            {
                _379 = card.extent.y;
            }
            _371 = _379;
        }
        float ev = _371;
        float _392;
        if (eu > 0.0001f)
        {
            _392 = pad_m / (eu * 2.0f);
        }
        else
        {
            _392 = 0.0f;
        }
        padU = _392;
        float _403;
        if (ev > 0.0001f)
        {
            _403 = pad_m / (ev * 2.0f);
        }
        else
        {
            _403 = 0.0f;
        }
        padV = _403;
    }
    if (((((u < ((-margin_uv) - padU)) || (u >= ((1.0f + margin_uv) + padU))) || (v < ((-margin_uv) - padV))) || (v >= ((1.0f + margin_uv) + padV))))
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999999f);
    v = clamp(v, 0.0f, 0.999999f);
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
    bool _487 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    out_uv = param_2;
    return _487;
}

bool worldToCardUV(float3 world_pos, SurfaceCacheCard card, out float2 out_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 0.0f;
    float2 param_2;
    bool _497 = worldToCardUVMargin(param, param_1, param_2, param_3);
    out_uv = param_2;
    return _497;
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card)
{
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5f) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5f) / float(card.resolution);
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
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

bool ddgiMeasuredOwnerCandidate(SurfaceCacheCard card, uint owner, bool ownerKnown, float3 hitPos, float3 geometricN, float3 rayDir, out float3 radiance, out float key, out uint reject)
{
    radiance = 0.0f.xxx;
    key = 1000000015047466219876688855040.0f;
    reject = 11u;
    if (((!ownerKnown) || (card.mesh_instance_id != owner)))
    {
        return false;
    }
    if ((((((((((any(isnan(hitPos))) || (any(isinf(hitPos)))) || (any(isnan(geometricN)))) || (any(isinf(geometricN)))) || (dot(geometricN, geometricN) < 0.25f)) || (dot(geometricN, rayDir) >= 0.0f)) || (card.resolution == 0u)) || ((card.axis_direction & 255u) > 2u)) || (((card.axis_direction >> 8u) & 255u) > 1u)))
    {
        return false;
    }
    float3 n = normalize(geometricN);
    SurfaceCacheCard param = card;
    float3 face = scCardFaceNormal(param);
    if (((dot(face, n) > (-0.5f)) || (dot(face, -rayDir) >= 0.0f)))
    {
        reject = 12u;
        return false;
    }
    float3 param_1 = hitPos;
    SurfaceCacheCard param_2 = card;
    float2 param_3;
    bool _654 = worldToCardUV(param_1, param_2, param_3);
    float2 uv = param_3;
    if ((((!_654) || (any(isnan(uv)))) || (any(isinf(uv)))))
    {
        reject = 13u;
        return false;
    }
    reject = 14u;

    int2 ds = int2(textureDimensions(depth_atlas, 0u));

    int2 ls = int2(textureDimensions(lighting_atlas, 0u));
    if (((((((any(bool2(ds.x <= int2(0, 0).x, ds.y <= int2(0, 0).y))) || (any(bool2(ds.x != ls.x, ds.y != ls.y)))) || (card.atlas_offset_x >= uint(ds.x))) || (card.atlas_offset_y >= uint(ds.y))) || (card.resolution > (uint(ds.x) - card.atlas_offset_x))) || (card.resolution > (uint(ds.y) - card.atlas_offset_y))))
    {
        return false;
    }
    uint2 texel = uint2(clamp(uv, float2(float(card.atlas_offset_x), float(card.atlas_offset_y)), float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth = depth_atlas.Load(int3(int2(texel), 0)).x;
    float4 light = lighting_atlas.Load(int3(int2(texel), 0));
    bool _795 = (isnan(depth)) || (isinf(depth));
    if (((((_795 || (depth < 0.0f)) || (any(isnan(light)))) || (any(isinf(light)))) || (light.w <= 0.5f)))
    {
        reject = 14u;
        return false;
    }
    uint2 param_4 = texel;
    float param_5 = depth;
    SurfaceCacheCard param_6 = card;
    float3 captured = cardTexelToWorld(param_4, param_5, param_6);
    float3 delta = captured - hitPos;
    float along = dot(delta, n);
    float footprint = (2.0f * length(card.extent.xyz)) / float(card.resolution);
    if (((((any(isnan(captured))) || (any(isinf(captured)))) || (abs(along) > 0.002f)) || (length(delta - (n * along)) > max(footprint, 0.002f))))
    {
        reject = 15u;
        return false;
    }
    radiance = light.xyz;
    key = length(delta - (n * along)) + abs(along);
    reject = 0u;
    return true;
}

[numthreads(8, 1, 1)]
void main(StageInput input)
{
    uint index = input.dispatchThreadID.x;
    uint count = query.Load(0) & 2147483647u;
    bool compareDDGI = (query.Load(0) & 2147483648u) != 0u;
    if (index >= count)
    {
        return;
    }
    float4 origin = asfloat(query.Load4((index * 2u) * 16 + 16));
    float4 direction = asfloat(query.Load4(((index * 2u) + 1u) * 16 + 16));
    float t = direction.w;
    uint owner = 4294967295u;
    uint primitive = 4294967295u;
    bool _2362 = ((((((!any(isnan(origin))) && (!any(isinf(origin)))) && (!any(isnan(direction)))) && (!any(isinf(direction)))) && (abs(dot(direction.xyz, direction.xyz) - 1.0f) <= 2e-05f)) && (origin.w >= 0.0f)) && (direction.w > origin.w);
    bool valid = _2362;
    int _2365;
    if (valid)
    {
        float3 param = origin.xyz;
        float3 param_1 = direction.xyz;
        float param_2 = origin.w;
        float param_3 = direction.w;
        float param_4;
        uint param_5;
        uint param_6;
        int _2383 = geometryFirstHit(param, param_1, param_2, param_3, param_4, param_5, param_6);
        t = param_4;
        owner = param_5;
        primitive = param_6;
        _2365 = _2383;
    }
    else
    {
        _2365 = -1;
    }
    int status = _2365;
    uint base = index * 4u;
    result.Store4(base * 16 + 0, asuint(origin));
    result.Store4((base + 1u) * 16 + 0, asuint(direction));
    result.Store4((base + 2u) * 16 + 0, asuint(float4(float(status), t, asfloat(owner), asfloat(primitive))));
    result.Store4((base + 3u) * 16 + 0, asuint(float4(asfloat(index), asfloat(query.Load(4)), asfloat(query.Load(8)), asfloat(query.Load(12)))));
    if (compareDDGI)
    {
        float3 param_7 = origin.xyz;
        float3 param_8 = direction.xyz;
        float param_9 = origin.w;
        float param_10 = direction.w;
        float param_11;
        uint param_12;
        uint param_13;
        float3 param_14;
        float3 param_15;
        float3 param_16;
        float3 param_17;
        int _2460 = geometryFirstHit(param_7, param_8, param_9, param_10, param_11, param_12, param_13, param_14, param_15, param_16, param_17);
        float dist = param_11;
        uint hitOwner = param_12;
        uint hitPrimitive = param_13;
        float3 normal = param_14;
        float3 a = param_15;
        float3 e1 = param_16;
        float3 e2 = param_17;
        int outcome = _2460;
        uint tail = (count * 4u) + (index * 2u);
        result.Store4(tail * 16 + 0, asuint(float4(float(outcome), dist, asfloat(hitOwner), asfloat(hitPrimitive))));
        result.Store4((tail + 1u) * 16 + 0, asuint(float4(normal, 0.0f)));
        uint bestCard = 4294967295u;
        uint seen = 0u;
        uint accepted = 0u;
        uint deepestReject = 0u;
        float bestKey = 1000000015047466219876688855040.0f;
        float3 light = 0.0f.xxx;
        bool _2499 = outcome == 1;
        bool _2512;
        if (_2499)
        {
            uint _2508;
            lookup.GetDimensions(_2508);
            _2508 = (_2508 - 0) / 48;
            _2512 = hitOwner < uint(int(_2508));
        }
        else
        {
            _2512 = _2499;
        }
        if (_2512)
        {
            uint first = lookup.Load(hitOwner * 48 + 40);
            uint end = lookup.Load(hitOwner * 48 + 44);
            bool _2525 = first <= end;
            bool _2538;
            if (_2525)
            {
                uint _2534;
                cards.GetDimensions(_2534);
                _2534 = (_2534 - 0) / 64;
                _2538 = end <= uint(int(_2534));
            }
            else
            {
                _2538 = _2525;
            }
            if (_2538)
            {
                SurfaceCacheCard card;
                float3 param_24;
                float param_25;
                uint param_26;
                for (uint ci = first; (ci < end) && (seen < 16u); ci++)
                {
                    SurfaceCacheCard _2559;
                    _2559.center = asfloat(cards.Load4(ci * 64));
                    _2559.extent = asfloat(cards.Load4(ci * 64 + 16));
                    _2559.axis_direction = cards.Load(ci * 64 + 32);
                    _2559.resolution = cards.Load(ci * 64 + 36);
                    _2559.atlas_offset_x = cards.Load(ci * 64 + 40);
                    _2559.atlas_offset_y = cards.Load(ci * 64 + 44);
                    _2559.mesh_instance_id = cards.Load(ci * 64 + 48);
                    _2559.static_mesh_slot = cards.Load(ci * 64 + 52);
                    _2559.flags = cards.Load(ci * 64 + 56);
                    _2559.page_card = cards.Load(ci * 64 + 60);
                    card.center = _2559.center;
                    card.extent = _2559.extent;
                    card.axis_direction = _2559.axis_direction;
                    card.resolution = _2559.resolution;
                    card.atlas_offset_x = _2559.atlas_offset_x;
                    card.atlas_offset_y = _2559.atlas_offset_y;
                    card.mesh_instance_id = _2559.mesh_instance_id;
                    card.static_mesh_slot = _2559.static_mesh_slot;
                    card.flags = _2559.flags;
                    card.page_card = _2559.page_card;
                    if (card.mesh_instance_id != hitOwner)
                    {
                        continue;
                    }
                    seen++;
                    SurfaceCacheCard param_18 = card;
                    uint param_19 = hitOwner;
                    bool param_20 = true;
                    float3 param_21 = origin.xyz + (direction.xyz * dist);
                    float3 param_22 = normal;
                    float3 param_23 = direction.xyz;
                    bool _2615 = ddgiMeasuredOwnerCandidate(param_18, param_19, param_20, param_21, param_22, param_23, param_24, param_25, param_26);
                    float3 candidate = param_24;
                    float key = param_25;
                    uint reject = param_26;
                    if (_2615)
                    {
                        accepted++;
                        if (key < bestKey)
                        {
                            bestKey = key;
                            bestCard = ci;
                            light = candidate;
                        }
                    }
                    else
                    {
                        deepestReject = max(deepestReject, reject);
                    }
                }
            }
        }
        uint surface = (count * 6u) + (index * 2u);
        result.Store4(surface * 16 + 0, asuint(float4(light, asfloat(bestCard))));
        result.Store4((surface + 1u) * 16 + 0, asuint(asfloat(uint4(seen, accepted, deepestReject, index))));
    }
}
