// Vulkan reference: EngineTest/shaders/Vulkan/SCBoundGeometryWitness.comp (main).
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

ByteAddressBuffer geometryTop : register(t13, space0);
ByteAddressBuffer geometryInstances : register(t16, space0);
ByteAddressBuffer geometryBottom : register(t14, space0);
ByteAddressBuffer geometryTriangles : register(t15, space0);
ByteAddressBuffer query : register(t0, space0);
RWByteAddressBuffer result : register(u1, space0);

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
    precise float _72 = c * d;
    float cd = _72;
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
    bool _282;
    if (false)
    {
        _282 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _282 = false;
    }
    bool _289;
    if (_282)
    {
        _289 = !any(isnan(d));
    }
    else
    {
        _289 = _282;
    }
    bool _296;
    if (_289)
    {
        _296 = !any(isinf(d));
    }
    else
    {
        _296 = _289;
    }
    bool topNonzero = _296;
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
        uint _314 = tp;
        uint _315 = _314 - 1u;
        tp = _315;
        uint at = ts[_315];
        uint _324;
        geometryTop.GetDimensions(_324);
        _324 = (_324 - 0) / 48;
        if (at >= uint(int(_324)))
        {
            return -1;
        }
        GeometryNode _336;
        _336.lo = asfloat(geometryTop.Load4(at * 48));
        _336.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _336.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _336.lo;
        n.hi = _336.hi;
        n.links = _336.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _360 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _360;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _372 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _372;
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
            uint _389 = tp;
            tp = _389 + 1u;
            ts[_389] = n.links.x;
            uint _394 = tp;
            tp = _394 + 1u;
            ts[_394] = n.links.y;
            continue;
        }
        uint _407;
        geometryInstances.GetDimensions(_407);
        _407 = (_407 - 0) / 64;
        if (n.links.z >= uint(int(_407)))
        {
            return -1;
        }
        GeometryInstance _421;
        _421.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64));
        _421.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _421.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _421.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _421.inverse0;
        inst.inverse1 = _421.inverse1;
        inst.inverse2 = _421.inverse2;
        inst.identity = _421.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        if (((((any(isnan(lo))) || (any(isinf(lo)))) || (any(isnan(ld)))) || (any(isinf(ld)))))
        {
            return -1;
        }
        bool _508;
        if (false)
        {
            _508 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _508 = false;
        }
        bool bottomNonzero = _508;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _521 = bp;
            uint _522 = _521 - 1u;
            bp = _522;
            at = bs[_522];
            uint _530;
            geometryBottom.GetDimensions(_530);
            _530 = (_530 - 0) / 48;
            if (at >= uint(int(_530)))
            {
                return -1;
            }
            GeometryNode _540;
            _540.lo = asfloat(geometryBottom.Load4(at * 48));
            _540.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _540.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _540.lo;
            b.hi = _540.hi;
            b.links = _540.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _561 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _561;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _573 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _573;
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
                uint _590 = bp;
                bp = _590 + 1u;
                bs[_590] = b.links.x;
                uint _595 = bp;
                bp = _595 + 1u;
                bs[_595] = b.links.y;
                continue;
            }
            uint _608;
            geometryTriangles.GetDimensions(_608);
            _608 = (_608 - 0) / 48;
            bool _611 = b.links.z > uint(int(_608));
            bool _624;
            if (!_611)
            {
                uint _617;
                geometryTriangles.GetDimensions(_617);
                _617 = (_617 - 0) / 48;
                _624 = b.links.w > (uint(int(_617)) - b.links.z);
            }
            else
            {
                _624 = _611;
            }
            if (_624)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _647;
                _647.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48));
                _647.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _647.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _647.a;
                tri.e1 = _647.e1;
                tri.e2 = _647.e2;
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
                bool _776 = (bary < 0.0f) || ((u + bary) > 1.0f);
                if (((_776 || (t < nearT)) || (t > bestT)))
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _801;
                if (found)
                {
                    _801 = t == bestT;
                }
                else
                {
                    _801 = found;
                }
                bool _822;
                if (_801)
                {
                    bool _807 = inst.identity.y > owner;
                    bool _821;
                    if (!_807)
                    {
                        bool _820 = (inst.identity.y == owner) && (primitive >= bestPrimitive);
                        _821 = _820;
                    }
                    else
                    {
                        _821 = _807;
                    }
                    _822 = _821;
                }
                else
                {
                    _822 = _801;
                }
                if (_822)
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
    bool _842;
    if (false)
    {
        _842 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _842 = false;
    }
    bool _849;
    if (_842)
    {
        _849 = !any(isnan(d));
    }
    else
    {
        _849 = _842;
    }
    bool _856;
    if (_849)
    {
        _856 = !any(isinf(d));
    }
    else
    {
        _856 = _849;
    }
    bool topNonzero = _856;
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
        uint _870 = topVisited;
        topVisited = _870 + 1u;
        uint _872;
        geometryTop.GetDimensions(_872);
        _872 = (_872 - 0) / 48;
        if (_870 >= uint(int(_872)))
        {
            return -1;
        }
        uint _880 = tp;
        uint _881 = _880 - 1u;
        tp = _881;
        uint at = ts[_881];
        uint _885;
        geometryTop.GetDimensions(_885);
        _885 = (_885 - 0) / 48;
        if (at >= uint(int(_885)))
        {
            return -1;
        }
        GeometryNode _895;
        _895.lo = asfloat(geometryTop.Load4(at * 48));
        _895.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _895.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _895.lo;
        n.hi = _895.hi;
        n.links = _895.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _916 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _916;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _928 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _928;
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
            uint _945 = tp;
            tp = _945 + 1u;
            ts[_945] = n.links.x;
            uint _950 = tp;
            tp = _950 + 1u;
            ts[_950] = n.links.y;
            continue;
        }
        uint _958;
        geometryInstances.GetDimensions(_958);
        _958 = (_958 - 0) / 64;
        if (n.links.z >= uint(int(_958)))
        {
            return -1;
        }
        GeometryInstance _969;
        _969.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64));
        _969.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _969.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _969.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _969.inverse0;
        inst.inverse1 = _969.inverse1;
        inst.inverse2 = _969.inverse2;
        inst.identity = _969.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        if (((((any(isnan(lo))) || (any(isinf(lo)))) || (any(isnan(ld)))) || (any(isinf(ld)))))
        {
            return -1;
        }
        bool _1054;
        if (false)
        {
            _1054 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _1054 = false;
        }
        bool bottomNonzero = _1054;
        uint bottomVisited = 0u;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _1068 = bottomVisited;
            bottomVisited = _1068 + 1u;
            uint _1070;
            geometryBottom.GetDimensions(_1070);
            _1070 = (_1070 - 0) / 48;
            if (_1068 >= uint(int(_1070)))
            {
                return -1;
            }
            uint _1077 = bp;
            uint _1078 = _1077 - 1u;
            bp = _1078;
            at = bs[_1078];
            uint _1082;
            geometryBottom.GetDimensions(_1082);
            _1082 = (_1082 - 0) / 48;
            if (at >= uint(int(_1082)))
            {
                return -1;
            }
            GeometryNode _1092;
            _1092.lo = asfloat(geometryBottom.Load4(at * 48));
            _1092.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1092.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _1092.lo;
            b.hi = _1092.hi;
            b.links = _1092.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _1113 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _1113;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _1125 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _1125;
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
                uint _1142 = bp;
                bp = _1142 + 1u;
                bs[_1142] = b.links.x;
                uint _1147 = bp;
                bp = _1147 + 1u;
                bs[_1147] = b.links.y;
                continue;
            }
            uint _1155;
            geometryTriangles.GetDimensions(_1155);
            _1155 = (_1155 - 0) / 48;
            bool _1158 = b.links.z > uint(int(_1155));
            bool _1171;
            if (!_1158)
            {
                uint _1164;
                geometryTriangles.GetDimensions(_1164);
                _1164 = (_1164 - 0) / 48;
                _1171 = b.links.w > (uint(int(_1164)) - b.links.z);
            }
            else
            {
                _1171 = _1158;
            }
            if (_1171)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _1191;
                _1191.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48));
                _1191.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _1191.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _1191.a;
                tri.e1 = _1191.e1;
                tri.e2 = _1191.e2;
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
                bool _1320 = (bary < 0.0f) || ((u + bary) > 1.0f);
                if (((_1320 || (t < nearT)) || (t > bestT)))
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bool _1345;
                if (found)
                {
                    _1345 = t == bestT;
                }
                else
                {
                    _1345 = found;
                }
                bool _1366;
                if (_1345)
                {
                    bool _1351 = inst.identity.y > owner;
                    bool _1365;
                    if (!_1351)
                    {
                        bool _1364 = (inst.identity.y == owner) && (primitive >= bestPrimitive);
                        _1365 = _1364;
                    }
                    else
                    {
                        _1365 = _1351;
                    }
                    _1366 = _1365;
                }
                else
                {
                    _1366 = _1345;
                }
                if (_1366)
                {
                    continue;
                }
                owner = inst.identity.y;
                bestPrimitive = primitive;
                bestT = t;
                found = true;
                float3 worldN = float3(dot(float3(inst.inverse0.x, inst.inverse1.x, inst.inverse2.x), normal), dot(float3(inst.inverse0.y, inst.inverse1.y, inst.inverse2.y), normal), dot(float3(inst.inverse0.z, inst.inverse1.z, inst.inverse2.z), normal));
                float normalScale = max(abs(worldN.x), max(abs(worldN.y), abs(worldN.z)));
                bool _1424 = (any(isnan(worldN))) || (any(isinf(worldN)));
                if (_1424 || (normalScale <= 0.0f))
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
    bool _1549 = ((((((!any(isnan(origin))) && (!any(isinf(origin)))) && (!any(isnan(direction)))) && (!any(isinf(direction)))) && (abs(dot(direction.xyz, direction.xyz) - 1.0f) <= 2e-05f)) && (origin.w >= 0.0f)) && (direction.w > origin.w);
    bool valid = _1549;
    int _1553;
    if (valid)
    {
        float3 param = origin.xyz;
        float3 param_1 = direction.xyz;
        float param_2 = origin.w;
        float param_3 = direction.w;
        float param_4;
        uint param_5;
        uint param_6;
        int _1571 = geometryFirstHit(param, param_1, param_2, param_3, param_4, param_5, param_6);
        t = param_4;
        owner = param_5;
        primitive = param_6;
        _1553 = _1571;
    }
    else
    {
        _1553 = -1;
    }
    int status = _1553;
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
        int _1648 = geometryFirstHit(param_7, param_8, param_9, param_10, param_11, param_12, param_13, param_14, param_15, param_16, param_17);
        float dist = param_11;
        uint hitOwner = param_12;
        uint hitPrimitive = param_13;
        float3 normal = param_14;
        float3 a = param_15;
        float3 e1 = param_16;
        float3 e2 = param_17;
        int outcome = _1648;
        uint tail = (count * 4u) + (index * 2u);
        result.Store4(tail * 16 + 0, asuint(float4(float(outcome), dist, asfloat(hitOwner), asfloat(hitPrimitive))));
        result.Store4((tail + 1u) * 16 + 0, asuint(float4(normal, 0.0f)));
    }
}
