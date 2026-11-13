struct GeometryNode
{
    float4 lo;
    float4 hi;
    uint4 links;
};

struct Receiver
{
    float3 p;
    float3 ng;
    float3 ns;
    float3 v;
    float roughness;
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

cbuffer VisibilityCB : register(b4, space0)
{
    row_major float4x4 cb_inverseViewProjection : packoffset(c0);
    float4 cb_camera : packoffset(c4);
    uint4 cb_dimensions : packoffset(c5);
    uint4 cb_policy : packoffset(c6);
    float4 cb_parameters : packoffset(c7);
};

ByteAddressBuffer geometryTop : register(t13, space0);
ByteAddressBuffer geometryInstances : register(t16, space0);
ByteAddressBuffer geometryBottom : register(t14, space0);
ByteAddressBuffer geometryTriangles : register(t15, space0);
Texture2D<float4> depthTexture : register(t0, space0);
Texture2D<float4> normalTexture : register(t1, space0);
Texture2D<float4> ormTexture : register(t2, space0);
RWTexture2D<float4> outputVisibility : register(u3, space0);
Texture2D<float4> halfVisibility : register(t5, space0);

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

bool finite1(float v)
{
    bool _265 = !isnan(v);
    bool _271;
    if (_265)
    {
        _271 = !isinf(v);
    }
    else
    {
        _271 = _265;
    }
    return _271;
}

bool finite3(float3 v)
{
    bool _253 = !any(isnan(v));
    bool _260;
    if (_253)
    {
        _260 = !any(isinf(v));
    }
    else
    {
        _260 = _253;
    }
    return _260;
}

bool pointAt(int2 pixel, inout float3 p)
{
    int2 size = int2(cb_dimensions.xy);
    bool _336 = any(bool2(pixel.x < int2(0, 0).x, pixel.y < int2(0, 0).y));
    bool _344;
    if (!_336)
    {
        _344 = any(bool2(pixel.x >= size.x, pixel.y >= size.y));
    }
    else
    {
        _344 = _336;
    }
    if (_344)
    {
        return false;
    }
    float z = depthTexture.Load(int3(pixel, 0)).x;
    float param = z;
    if (((!finite1(param)) || (z < 0.0f)) || (z >= 0.999998986721038818359375f))
    {
        return false;
    }
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(size);
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f)) - float2(cb_camera.w, asfloat(cb_policy.z));
    bool _402 = any(isnan(ndc));
    bool _409;
    if (!_402)
    {
        _409 = any(isinf(ndc));
    }
    else
    {
        _409 = _402;
    }
    if (_409)
    {
        return false;
    }
    float4 h = mul(float4(ndc, z, 1.0f), cb_inverseViewProjection);
    float param_1 = h.w;
    bool _428 = !finite1(param_1);
    bool _437;
    if (!_428)
    {
        _437 = abs(h.w) < 9.9999999392252902907785028219223e-09f;
    }
    else
    {
        _437 = _428;
    }
    if (_437)
    {
        return false;
    }
    p = h.xyz / h.w.xxx;
    float3 param_2 = p;
    return finite3(param_2);
}

bool receiver(int2 pixel, inout Receiver r)
{
    if ((cb_policy.y & 2147483648u) != 0u)
    {
        return false;
    }
    uint _461_dummy_parameter;
    int2 _461 = int2(spvTextureSize(depthTexture, uint(0), _461_dummy_parameter));
    int2 _465 = int2(cb_dimensions.xy);
    bool _467 = any(bool2(_461.x != _465.x, _461.y != _465.y));
    bool _480;
    if (!_467)
    {
        uint _473_dummy_parameter;
        int2 _473 = int2(spvTextureSize(normalTexture, uint(0), _473_dummy_parameter));
        int2 _477 = int2(cb_dimensions.xy);
        _480 = any(bool2(_473.x != _477.x, _473.y != _477.y));
    }
    else
    {
        _480 = _467;
    }
    bool _493;
    if (!_480)
    {
        uint _486_dummy_parameter;
        int2 _486 = int2(spvTextureSize(ormTexture, uint(0), _486_dummy_parameter));
        int2 _490 = int2(cb_dimensions.xy);
        _493 = any(bool2(_486.x != _490.x, _486.y != _490.y));
    }
    else
    {
        _493 = _480;
    }
    if (_493)
    {
        return false;
    }
    int2 param = pixel;
    float3 param_1;
    bool _500 = pointAt(param, param_1);
    r.p = param_1;
    if (!_500)
    {
        return false;
    }
    float3 view = cb_camera.xyz - r.p;
    float3 param_2 = view;
    bool _518 = !finite3(param_2);
    bool _527;
    if (!_518)
    {
        _527 = dot(view, view) < 1.0000000133514319600180897396058e-10f;
    }
    else
    {
        _527 = _518;
    }
    if (_527)
    {
        return false;
    }
    r.v = normalize(view);
    int2 param_3 = pixel - int2(1, 0);
    float3 param_4;
    bool _542 = pointAt(param_3, param_4);
    float3 px0 = param_4;
    bool x0 = _542;
    int2 param_5 = pixel + int2(1, 0);
    float3 param_6;
    bool _550 = pointAt(param_5, param_6);
    float3 px1 = param_6;
    bool x1 = _550;
    int2 param_7 = pixel - int2(0, 1);
    float3 param_8;
    bool _559 = pointAt(param_7, param_8);
    float3 py0 = param_8;
    bool y0 = _559;
    int2 param_9 = pixel + int2(0, 1);
    float3 param_10;
    bool _567 = pointAt(param_9, param_10);
    float3 py1 = param_10;
    bool y1 = _567;
    bool _573 = (!x0) && (!x1);
    bool _582;
    if (!_573)
    {
        _582 = (!y0) && (!y1);
    }
    else
    {
        _582 = _573;
    }
    if (_582)
    {
        return false;
    }
    bool _605;
    if (x0)
    {
        bool _591 = !x1;
        bool _604;
        if (!_591)
        {
            _604 = distance(px0, r.p) < distance(px1, r.p);
        }
        else
        {
            _604 = _591;
        }
        _605 = _604;
    }
    else
    {
        _605 = x0;
    }
    float3 _606;
    if (_605)
    {
        _606 = r.p - px0;
    }
    else
    {
        _606 = px1 - r.p;
    }
    float3 dx = _606;
    bool _638;
    if (y0)
    {
        bool _624 = !y1;
        bool _637;
        if (!_624)
        {
            _637 = distance(py0, r.p) < distance(py1, r.p);
        }
        else
        {
            _637 = _624;
        }
        _638 = _637;
    }
    else
    {
        _638 = y0;
    }
    float3 _639;
    if (_638)
    {
        _639 = r.p - py0;
    }
    else
    {
        _639 = py1 - r.p;
    }
    float3 dy = _639;
    float limit = max(0.0199999995529651641845703125f, length(view) * 0.0199999995529651641845703125f);
    bool _661 = length(dx) > limit;
    bool _669;
    if (!_661)
    {
        _669 = length(dy) > limit;
    }
    else
    {
        _669 = _661;
    }
    if (_669)
    {
        return false;
    }
    float3 n = cross(dx, dy);
    float s = max(abs(n.x), max(abs(n.y), abs(n.z)));
    float3 param_11 = n;
    if ((!finite3(param_11)) || (s < 1.000000016862383526387164645044e-16f))
    {
        return false;
    }
    r.ng = normalize(n / s.xxx);
    if (dot(r.ng, r.v) < 0.0f)
    {
        r.ng = -r.ng;
    }
    float3 ns = (normalTexture.Load(int3(pixel, 0)).xyz * 2.0f) - 1.0f.xxx;
    float3 param_12 = ns;
    bool _729 = !finite3(param_12);
    bool _738;
    if (!_729)
    {
        _738 = dot(ns, ns) < 0.25f;
    }
    else
    {
        _738 = _729;
    }
    if (_738)
    {
        return false;
    }
    r.ns = normalize(ns);
    bool _751 = dot(r.ns, r.v) <= 9.9999997473787516355514526367188e-06f;
    bool _761;
    if (!_751)
    {
        _761 = dot(r.ns, r.ng) <= 0.0f;
    }
    else
    {
        _761 = _751;
    }
    if (_761)
    {
        return false;
    }
    float rough = ormTexture.Load(int3(pixel, 0)).y;
    float param_13 = rough;
    if (((!finite1(param_13)) || (rough < 0.0f)) || (rough > 1.0f))
    {
        return false;
    }
    r.roughness = max(rough, 0.039999999105930328369140625f);
    return true;
}

uint hash32(inout uint x)
{
    x ^= (x >> 16u);
    x *= 2146121005u;
    x ^= (x >> 15u);
    x *= 2221713035u;
    return x ^ (x >> 16u);
}

float2 random2(uint seed)
{
    uint param = seed ^ 1757159915u;
    uint _300 = hash32(param);
    uint param_1 = seed ^ 48610963u;
    uint _308 = hash32(param_1);
    return (float2(float(_300 >> 8u), float(_308 >> 8u)) + 0.5f.xx) / 16777216.0f.xx;
}

float3 sampleVisibleGGX(float3 view, float alpha, float2 xi)
{
    float3 vh = normalize(float3(view.xy * alpha, view.z));
    float3 _1757;
    if (vh.z < 0.999989986419677734375f)
    {
        _1757 = normalize(cross(float3(0.0f, 0.0f, 1.0f), vh));
    }
    else
    {
        _1757 = float3(1.0f, 0.0f, 0.0f);
    }
    float3 t1 = _1757;
    float3 t2 = cross(vh, t1);
    float radius = sqrt(xi.x);
    float phi = 6.283185482025146484375f * xi.y;
    float a = radius * cos(phi);
    float b = radius * sin(phi);
    float s = 0.5f * (1.0f + vh.z);
    b = ((1.0f - s) * sqrt(max(0.0f, 1.0f - (a * a)))) + (s * b);
    float3 nh = ((t1 * a) + (t2 * b)) + (vh * sqrt(max(0.0f, (1.0f - (a * a)) - (b * b))));
    return normalize(float3(nh.xy * alpha, max(0.0f, nh.z)));
}

bool environmentNodeValid(GeometryNode node)
{
    float3 param = node.lo.xyz;
    bool _794 = finite3(param);
    bool _802;
    if (_794)
    {
        float3 param_1 = node.hi.xyz;
        _802 = finite3(param_1);
    }
    else
    {
        _802 = _794;
    }
    bool _813;
    if (_802)
    {
        _813 = all(bool3(node.lo.xyz.x <= node.hi.xyz.x, node.lo.xyz.y <= node.hi.xyz.y, node.lo.xyz.z <= node.hi.xyz.z));
    }
    else
    {
        _813 = _802;
    }
    return _813;
}

bool geometryBox(GeometryNode n, float3 o, float3 d, inout float nearT, inout float farT)
{
    for (uint k = 0u; k < 3u; k++)
    {
        if (d[k] == 0.0f)
        {
            bool _186 = o[k] < n.lo[k];
            bool _198;
            if (!_186)
            {
                _198 = o[k] > n.hi[k];
            }
            else
            {
                _198 = _186;
            }
            if (_198)
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
    precise float _100 = c * d;
    float cd = _100;
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

int environmentAnyHit(float3 origin, float3 direction, float nearT, float farT)
{
    float3 param = origin;
    bool _819 = !finite3(param);
    bool _827;
    if (!_819)
    {
        float3 param_1 = direction;
        _827 = !finite3(param_1);
    }
    else
    {
        _827 = _819;
    }
    bool _835;
    if (!_827)
    {
        float param_2 = nearT;
        _835 = !finite1(param_2);
    }
    else
    {
        _835 = _827;
    }
    bool _843;
    if (!_835)
    {
        float param_3 = farT;
        _843 = !finite1(param_3);
    }
    else
    {
        _843 = _835;
    }
    bool _850 = (_843 || (nearT < 0.0f)) || (farT <= nearT);
    bool _858;
    if (!_850)
    {
        _858 = dot(direction, direction) <= 0.0f;
    }
    else
    {
        _858 = _850;
    }
    if (_858)
    {
        return -1;
    }
    bool endpoint = false;
    uint topSize = 1u;
    uint topVisited = 0u;
    uint topStack[64];
    topStack[0] = 0u;
    GeometryNode node;
    GeometryInstance instance;
    uint bottomStack[64];
    GeometryNode bottom;
    GeometryTriangle _triangle;
    while (topSize != 0u)
    {
        uint _878 = topVisited;
        topVisited = _878 + uint(1);
        uint _885;
        geometryTop.GetDimensions(_885);
        _885 = (_885 - 0) / 48;
        if (_878 >= uint(int(_885)))
        {
            return -1;
        }
        uint _893 = topSize;
        uint _894 = _893 - uint(1);
        topSize = _894;
        uint at = topStack[_894];
        uint _898;
        geometryTop.GetDimensions(_898);
        _898 = (_898 - 0) / 48;
        if (at >= uint(int(_898)))
        {
            return -1;
        }
        GeometryNode _909;
        _909.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _909.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _909.links = geometryTop.Load4(at * 48 + 32);
        node.lo = _909.lo;
        node.hi = _909.hi;
        node.links = _909.links;
        GeometryNode param_4 = node;
        if (!environmentNodeValid(param_4))
        {
            return -1;
        }
        GeometryNode param_5 = node;
        float3 param_6 = origin;
        float3 param_7 = direction;
        float param_8 = nearT;
        float param_9 = farT;
        bool _934 = geometryBox(param_5, param_6, param_7, param_8, param_9);
        if (!_934)
        {
            continue;
        }
        if (node.links.w == 0u)
        {
            bool _946 = (topSize + 2u) > 64u;
            bool _956;
            if (!_946)
            {
                uint _952;
                geometryTop.GetDimensions(_952);
                _952 = (_952 - 0) / 48;
                _956 = node.links.x >= uint(int(_952));
            }
            else
            {
                _956 = _946;
            }
            bool _966;
            if (!_956)
            {
                uint _962;
                geometryTop.GetDimensions(_962);
                _962 = (_962 - 0) / 48;
                _966 = node.links.y >= uint(int(_962));
            }
            else
            {
                _966 = _956;
            }
            if (_966)
            {
                return -1;
            }
            uint _970 = topSize;
            topSize = _970 + uint(1);
            topStack[_970] = node.links.x;
            uint _975 = topSize;
            topSize = _975 + uint(1);
            topStack[_975] = node.links.y;
            continue;
        }
        bool _983 = node.links.w != 1u;
        bool _998;
        if (!_983)
        {
            uint _994;
            geometryInstances.GetDimensions(_994);
            _994 = (_994 - 0) / 64;
            _998 = node.links.z >= uint(int(_994));
        }
        else
        {
            _998 = _983;
        }
        if (_998)
        {
            return -1;
        }
        GeometryInstance _1009;
        _1009.inverse0 = asfloat(geometryInstances.Load4(node.links.z * 64 + 0));
        _1009.inverse1 = asfloat(geometryInstances.Load4(node.links.z * 64 + 16));
        _1009.inverse2 = asfloat(geometryInstances.Load4(node.links.z * 64 + 32));
        _1009.identity = geometryInstances.Load4(node.links.z * 64 + 48);
        instance.inverse0 = _1009.inverse0;
        instance.inverse1 = _1009.inverse1;
        instance.inverse2 = _1009.inverse2;
        instance.identity = _1009.identity;
        bool _1022 = any(isnan(instance.inverse0));
        bool _1030;
        if (!_1022)
        {
            _1030 = any(isinf(instance.inverse0));
        }
        else
        {
            _1030 = _1022;
        }
        bool _1038;
        if (!_1030)
        {
            _1038 = any(isnan(instance.inverse1));
        }
        else
        {
            _1038 = _1030;
        }
        bool _1046;
        if (!_1038)
        {
            _1046 = any(isinf(instance.inverse1));
        }
        else
        {
            _1046 = _1038;
        }
        bool _1054;
        if (!_1046)
        {
            _1054 = any(isnan(instance.inverse2));
        }
        else
        {
            _1054 = _1046;
        }
        bool _1062;
        if (!_1054)
        {
            _1062 = any(isinf(instance.inverse2));
        }
        else
        {
            _1062 = _1054;
        }
        if (_1062)
        {
            return -1;
        }
        float inverseDet = dot(instance.inverse0.xyz, cross(instance.inverse1.xyz, instance.inverse2.xyz));
        float param_10 = inverseDet;
        if ((!finite1(param_10)) || (inverseDet == 0.0f))
        {
            return -1;
        }
        float3 localOrigin = float3(dot(instance.inverse0, float4(origin, 1.0f)), dot(instance.inverse1, float4(origin, 1.0f)), dot(instance.inverse2, float4(origin, 1.0f)));
        float3 localDirection = float3(dot(instance.inverse0.xyz, direction), dot(instance.inverse1.xyz, direction), dot(instance.inverse2.xyz, direction));
        float directionSquared = dot(localDirection, localDirection);
        float3 param_11 = localOrigin;
        bool _1138 = !finite3(param_11);
        bool _1146;
        if (!_1138)
        {
            float3 param_12 = localDirection;
            _1146 = !finite3(param_12);
        }
        else
        {
            _1146 = _1138;
        }
        bool _1154;
        if (!_1146)
        {
            float param_13 = directionSquared;
            _1154 = !finite1(param_13);
        }
        else
        {
            _1154 = _1146;
        }
        if (_1154 || (directionSquared <= 0.0f))
        {
            return -1;
        }
        uint bottomSize = 1u;
        uint bottomVisited = 0u;
        bottomStack[0] = instance.identity.x;
        while (bottomSize != 0u)
        {
            uint _1174 = bottomVisited;
            bottomVisited = _1174 + uint(1);
            uint _1180;
            geometryBottom.GetDimensions(_1180);
            _1180 = (_1180 - 0) / 48;
            if (_1174 >= uint(int(_1180)))
            {
                return -1;
            }
            uint _1187 = bottomSize;
            uint _1188 = _1187 - uint(1);
            bottomSize = _1188;
            at = bottomStack[_1188];
            uint _1192;
            geometryBottom.GetDimensions(_1192);
            _1192 = (_1192 - 0) / 48;
            if (at >= uint(int(_1192)))
            {
                return -1;
            }
            GeometryNode _1202;
            _1202.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _1202.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1202.links = geometryBottom.Load4(at * 48 + 32);
            bottom.lo = _1202.lo;
            bottom.hi = _1202.hi;
            bottom.links = _1202.links;
            GeometryNode param_14 = bottom;
            if (!environmentNodeValid(param_14))
            {
                return -1;
            }
            GeometryNode param_15 = bottom;
            float3 param_16 = localOrigin;
            float3 param_17 = localDirection;
            float param_18 = nearT;
            float param_19 = farT;
            bool _1226 = geometryBox(param_15, param_16, param_17, param_18, param_19);
            if (!_1226)
            {
                continue;
            }
            if (bottom.links.w == 0u)
            {
                bool _1238 = (bottomSize + 2u) > 64u;
                bool _1248;
                if (!_1238)
                {
                    uint _1244;
                    geometryBottom.GetDimensions(_1244);
                    _1244 = (_1244 - 0) / 48;
                    _1248 = bottom.links.x >= uint(int(_1244));
                }
                else
                {
                    _1248 = _1238;
                }
                bool _1258;
                if (!_1248)
                {
                    uint _1254;
                    geometryBottom.GetDimensions(_1254);
                    _1254 = (_1254 - 0) / 48;
                    _1258 = bottom.links.y >= uint(int(_1254));
                }
                else
                {
                    _1258 = _1248;
                }
                if (_1258)
                {
                    return -1;
                }
                uint _1262 = bottomSize;
                bottomSize = _1262 + uint(1);
                bottomStack[_1262] = bottom.links.x;
                uint _1267 = bottomSize;
                bottomSize = _1267 + uint(1);
                bottomStack[_1267] = bottom.links.y;
                continue;
            }
            uint _1280;
            geometryTriangles.GetDimensions(_1280);
            _1280 = (_1280 - 0) / 48;
            bool _1283 = bottom.links.z > uint(int(_1280));
            bool _1296;
            if (!_1283)
            {
                uint _1289;
                geometryTriangles.GetDimensions(_1289);
                _1289 = (_1289 - 0) / 48;
                _1296 = bottom.links.w > (uint(int(_1289)) - bottom.links.z);
            }
            else
            {
                _1296 = _1283;
            }
            if (_1296)
            {
                return -1;
            }
            for (uint i = 0u; i < bottom.links.w; i++)
            {
                GeometryTriangle _1319;
                _1319.a = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 0));
                _1319.e1 = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 16));
                _1319.e2 = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 32));
                _triangle.a = _1319.a;
                _triangle.e1 = _1319.e1;
                _triangle.e2 = _1319.e2;
                float3 param_20 = _triangle.a.xyz;
                bool _1331 = !finite3(param_20);
                bool _1341;
                if (!_1331)
                {
                    float3 param_21 = _triangle.e1.xyz;
                    _1341 = !finite3(param_21);
                }
                else
                {
                    _1341 = _1331;
                }
                bool _1351;
                if (!_1341)
                {
                    float3 param_22 = _triangle.e2.xyz;
                    _1351 = !finite3(param_22);
                }
                else
                {
                    _1351 = _1341;
                }
                if (_1351)
                {
                    return -1;
                }
                float3 p = cross(localDirection, _triangle.e2.xyz);
                float det = dot(_triangle.e1.xyz, p);
                float param_23 = det;
                if (!finite1(param_23))
                {
                    return -1;
                }
                if (det == 0.0f)
                {
                    continue;
                }
                float3 delta = localOrigin - _triangle.a.xyz;
                float u = dot(delta, p) / det;
                float param_24 = u;
                if (!finite1(param_24))
                {
                    return -1;
                }
                if ((u < 0.0f) || (u > 1.0f))
                {
                    continue;
                }
                float3 q = cross(delta, _triangle.e1.xyz);
                float v = dot(localDirection, q) / det;
                float param_25 = v;
                if (!finite1(param_25))
                {
                    return -1;
                }
                bool _1426 = v < 0.0f;
                bool _1434;
                if (!_1426)
                {
                    _1434 = (u + v) > 1.0f;
                }
                else
                {
                    _1434 = _1426;
                }
                if (_1434)
                {
                    continue;
                }
                float3 param_26 = _triangle.e1.xyz;
                float3 param_27 = _triangle.e2.xyz;
                float3 normal = geometryTriangleNormal(param_26, param_27);
                float planeDet = dot(localDirection, normal);
                float3 param_28 = normal;
                bool _1455 = !finite3(param_28);
                bool _1463;
                if (!_1455)
                {
                    float param_29 = planeDet;
                    _1463 = !finite1(param_29);
                }
                else
                {
                    _1463 = _1455;
                }
                if (_1463 || (planeDet == 0.0f))
                {
                    return -1;
                }
                float t = dot(_triangle.a.xyz - localOrigin, normal) / planeDet;
                float param_30 = t;
                if (!finite1(param_30))
                {
                    return -1;
                }
                if ((t < nearT) || (t > farT))
                {
                    continue;
                }
                bool _1501 = t <= (nearT + 1.9999999494757503271102905273438e-05f);
                bool _1509;
                if (!_1501)
                {
                    _1509 = t >= (farT - 1.9999999494757503271102905273438e-05f);
                }
                else
                {
                    _1509 = _1501;
                }
                if (_1509)
                {
                    endpoint = true;
                    continue;
                }
                return 1;
            }
        }
    }
    return endpoint ? (-1) : 0;
}

int environmentRay(Receiver r, float3 direction)
{
    float3 param = direction;
    if (!finite3(param))
    {
        return 0;
    }
    if (dot(direction, r.ng) <= 0.0f)
    {
        return 1;
    }
    bool _1537 = cb_policy.x != 1u;
    bool _1544;
    if (!_1537)
    {
        uint _1541;
        geometryTop.GetDimensions(_1541);
        _1541 = (_1541 - 0) / 48;
        _1544 = int(_1541) == 0;
    }
    else
    {
        _1544 = _1537;
    }
    bool _1551;
    if (!_1544)
    {
        uint _1548;
        geometryBottom.GetDimensions(_1548);
        _1548 = (_1548 - 0) / 48;
        _1551 = int(_1548) == 0;
    }
    else
    {
        _1551 = _1544;
    }
    bool _1558;
    if (!_1551)
    {
        uint _1555;
        geometryTriangles.GetDimensions(_1555);
        _1555 = (_1555 - 0) / 48;
        _1558 = int(_1555) == 0;
    }
    else
    {
        _1558 = _1551;
    }
    bool _1565;
    if (!_1558)
    {
        uint _1562;
        geometryInstances.GetDimensions(_1562);
        _1562 = (_1562 - 0) / 64;
        _1565 = int(_1562) == 0;
    }
    else
    {
        _1565 = _1558;
    }
    if (_1565)
    {
        return 0;
    }
    GeometryNode _1571;
    _1571.lo = asfloat(geometryTop.Load4(0));
    _1571.hi = asfloat(geometryTop.Load4(16));
    _1571.links = geometryTop.Load4(32);
    GeometryNode root;
    root.lo = _1571.lo;
    root.hi = _1571.hi;
    root.links = _1571.links;
    float3 param_1 = root.lo.xyz;
    bool _1583 = !finite3(param_1);
    bool _1593;
    if (!_1583)
    {
        float3 param_2 = root.hi.xyz;
        _1593 = !finite3(param_2);
    }
    else
    {
        _1593 = _1583;
    }
    bool _1605;
    if (!_1593)
    {
        _1605 = any(bool3(root.lo.xyz.x > root.hi.xyz.x, root.lo.xyz.y > root.hi.xyz.y, root.lo.xyz.z > root.hi.xyz.z));
    }
    else
    {
        _1605 = _1593;
    }
    if (_1605)
    {
        return 0;
    }
    float3 origin = r.p + (r.ng * cb_parameters.x);
    float entry = 0.0f;
    float exit = 3.4028234663852885981170418348452e+38f;
    for (uint k = 0u; k < 3u; k++)
    {
        if (direction[k] == 0.0f)
        {
            bool _1641 = origin[k] < root.lo[k];
            bool _1652;
            if (!_1641)
            {
                _1652 = origin[k] > root.hi[k];
            }
            else
            {
                _1652 = _1641;
            }
            if (_1652)
            {
                return 2;
            }
        }
        else
        {
            float a = (root.lo[k] - origin[k]) / direction[k];
            float b = (root.hi[k] - origin[k]) / direction[k];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
            if (entry > exit)
            {
                return 2;
            }
        }
    }
    float param_3 = exit;
    if (!finite1(param_3))
    {
        return 0;
    }
    if (exit < 0.0f)
    {
        return 2;
    }
    float tmax = exit + max(0.001000000047497451305389404296875f, abs(exit) * 9.9999997473787516355514526367188e-06f);
    float3 param_4 = origin;
    float3 param_5 = direction;
    float param_6 = 0.0f;
    float param_7 = tmax;
    int hit = environmentAnyHit(param_4, param_5, param_6, param_7);
    int _1731;
    if (hit < 0)
    {
        _1731 = 0;
    }
    else
    {
        _1731 = (hit > 0) ? 1 : 2;
    }
    return _1731;
}

float4 traceLobe(Receiver r, int2 pixel)
{
    if (cb_policy.y == 1u)
    {
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    }
    if (cb_policy.x != 1u)
    {
        return float4(0.0f, 0.0f, 1.0f, 1.0f);
    }
    bool3 _1862 = (abs(r.ns.z) < 0.999000012874603271484375f).xxx;
    float3 t = normalize(cross(float3(_1862.x ? float3(0.0f, 0.0f, 1.0f).x : float3(1.0f, 0.0f, 0.0f).x, _1862.y ? float3(0.0f, 0.0f, 1.0f).y : float3(1.0f, 0.0f, 0.0f).y, _1862.z ? float3(0.0f, 0.0f, 1.0f).z : float3(1.0f, 0.0f, 0.0f).z), r.ns));
    float3 b = cross(r.ns, t);
    float3 v = float3(dot(r.v, t), dot(r.v, b), dot(r.v, r.ns));
    uint count = (r.roughness <= 0.0599999986588954925537109375f) ? 1u : 4u;
    float visible = 0.0f;
    float known = 0.0f;
    float total = 0.0f;
    bool anyUnknown = false;
    bool anyEscape = false;
    bool allEscape = true;
    float alpha = r.roughness * r.roughness;
    uint seed = (uint(pixel.x) * 2654435769u) ^ (uint(pixel.y) * 2246822507u);
    uint param = seed;
    float2 rotation = random2(param);
    float3 localL;
    for (uint i = 0u; i < count; i++)
    {
        float weight = 1.0f;
        if (count == 1u)
        {
            localL = float3(-v.xy, v.z);
        }
        else
        {
            float2 xi = frac(float2((float(i) + 0.5f) / 4.0f, (float(((i & 1u) * 2u) + (i >> 1u)) * 0.25f) + 0.125f) + rotation);
            float3 param_1 = v;
            float param_2 = alpha;
            float2 param_3 = xi;
            float3 h = sampleVisibleGGX(param_1, param_2, param_3);
            localL = reflect(-v, h);
            if (localL.z <= 0.0f)
            {
                continue;
            }
            float c = localL.z;
            weight = (2.0f * c) / (c + sqrt((alpha * alpha) + (((1.0f - (alpha * alpha)) * c) * c)));
        }
        float3 worldL = normalize(((t * localL.x) + (b * localL.y)) + (r.ns * localL.z));
        Receiver param_4 = r;
        float3 param_5 = worldL;
        int outcome = environmentRay(param_4, param_5);
        total += weight;
        anyUnknown = anyUnknown || (outcome == 0);
        anyEscape = anyEscape || (outcome == 2);
        allEscape = allEscape && (outcome == 2);
        if (outcome != 0)
        {
            known += weight;
        }
        if (outcome == 2)
        {
            visible += weight;
        }
    }
    if (total <= 0.0f)
    {
        return float4(0.0f, 1.0f, 0.0f, 1.0f);
    }
    float _2068;
    if (allEscape)
    {
        _2068 = 1.0f;
    }
    else
    {
        float _2073;
        if (anyEscape)
        {
            _2073 = clamp(visible / total, 0.0f, 1.0f);
        }
        else
        {
            _2073 = 0.0f;
        }
        _2068 = _2073;
    }
    float visibility = _2068;
    float _2085;
    if (anyUnknown)
    {
        _2085 = clamp(known / total, 0.0f, 1.0f);
    }
    else
    {
        _2085 = 1.0f;
    }
    float knownFraction = _2085;
    return float4(visibility, knownFraction, float(anyUnknown), 1.0f);
}

void comp_main()
{
    int2 pixel = int2(gl_GlobalInvocationID.xy);
    int2 _2112 = int2(cb_dimensions.zw);
    if (any(bool2(pixel.x >= _2112.x, pixel.y >= _2112.y)))
    {
        return;
    }
    int2 source = min(((pixel * int2(2, 2)) + int2(1, 1)), (int2(cb_dimensions.xy) - int2(1, 1)));
    int2 param = source;
    Receiver param_1;
    bool _2140 = receiver(param, param_1);
    Receiver r = param_1;
    float4 _2142;
    if (_2140)
    {
        Receiver param_2 = r;
        int2 param_3 = source;
        _2142 = traceLobe(param_2, param_3);
    }
    else
    {
        _2142 = float4(0.0f, 0.0f, 1.0f, 0.0f);
    }
    outputVisibility[pixel] = _2142;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
