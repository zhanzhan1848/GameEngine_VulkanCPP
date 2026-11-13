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

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

groupshared Receiver sourceReceiverCache[36];
groupshared uint sourceReceiverValid[36];

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

bool finite1(float v)
{
    bool _248 = !isnan(v);
    bool _254;
    if (_248)
    {
        _254 = !isinf(v);
    }
    else
    {
        _254 = _248;
    }
    return _254;
}

bool finite3(float3 v)
{
    bool _236 = !any(isnan(v));
    bool _243;
    if (_236)
    {
        _243 = !any(isinf(v));
    }
    else
    {
        _243 = _236;
    }
    return _243;
}

bool pointAt(int2 pixel, inout float3 p)
{
    int2 size = int2(cb_dimensions.xy);
    bool _273 = any(bool2(pixel.x < int2(0, 0).x, pixel.y < int2(0, 0).y));
    bool _281;
    if (!_273)
    {
        _281 = any(bool2(pixel.x >= size.x, pixel.y >= size.y));
    }
    else
    {
        _281 = _273;
    }
    if (_281)
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
    bool _342 = any(isnan(ndc));
    bool _349;
    if (!_342)
    {
        _349 = any(isinf(ndc));
    }
    else
    {
        _349 = _342;
    }
    if (_349)
    {
        return false;
    }
    float4 h = mul(float4(ndc, z, 1.0f), cb_inverseViewProjection);
    float param_1 = h.w;
    bool _368 = !finite1(param_1);
    bool _377;
    if (!_368)
    {
        _377 = abs(h.w) < 9.9999999392252902907785028219223e-09f;
    }
    else
    {
        _377 = _368;
    }
    if (_377)
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
    uint _401_dummy_parameter;
    int2 _401 = int2(spvTextureSize(depthTexture, uint(0), _401_dummy_parameter));
    int2 _405 = int2(cb_dimensions.xy);
    bool _407 = any(bool2(_401.x != _405.x, _401.y != _405.y));
    bool _420;
    if (!_407)
    {
        uint _413_dummy_parameter;
        int2 _413 = int2(spvTextureSize(normalTexture, uint(0), _413_dummy_parameter));
        int2 _417 = int2(cb_dimensions.xy);
        _420 = any(bool2(_413.x != _417.x, _413.y != _417.y));
    }
    else
    {
        _420 = _407;
    }
    bool _433;
    if (!_420)
    {
        uint _426_dummy_parameter;
        int2 _426 = int2(spvTextureSize(ormTexture, uint(0), _426_dummy_parameter));
        int2 _430 = int2(cb_dimensions.xy);
        _433 = any(bool2(_426.x != _430.x, _426.y != _430.y));
    }
    else
    {
        _433 = _420;
    }
    if (_433)
    {
        return false;
    }
    int2 param = pixel;
    float3 param_1;
    bool _440 = pointAt(param, param_1);
    r.p = param_1;
    if (!_440)
    {
        return false;
    }
    float3 view = cb_camera.xyz - r.p;
    float3 param_2 = view;
    bool _458 = !finite3(param_2);
    bool _467;
    if (!_458)
    {
        _467 = dot(view, view) < 1.0000000133514319600180897396058e-10f;
    }
    else
    {
        _467 = _458;
    }
    if (_467)
    {
        return false;
    }
    r.v = normalize(view);
    int2 param_3 = pixel - int2(1, 0);
    float3 param_4;
    bool _482 = pointAt(param_3, param_4);
    float3 px0 = param_4;
    bool x0 = _482;
    int2 param_5 = pixel + int2(1, 0);
    float3 param_6;
    bool _490 = pointAt(param_5, param_6);
    float3 px1 = param_6;
    bool x1 = _490;
    int2 param_7 = pixel - int2(0, 1);
    float3 param_8;
    bool _499 = pointAt(param_7, param_8);
    float3 py0 = param_8;
    bool y0 = _499;
    int2 param_9 = pixel + int2(0, 1);
    float3 param_10;
    bool _507 = pointAt(param_9, param_10);
    float3 py1 = param_10;
    bool y1 = _507;
    bool _513 = (!x0) && (!x1);
    bool _522;
    if (!_513)
    {
        _522 = (!y0) && (!y1);
    }
    else
    {
        _522 = _513;
    }
    if (_522)
    {
        return false;
    }
    bool _545;
    if (x0)
    {
        bool _531 = !x1;
        bool _544;
        if (!_531)
        {
            _544 = distance(px0, r.p) < distance(px1, r.p);
        }
        else
        {
            _544 = _531;
        }
        _545 = _544;
    }
    else
    {
        _545 = x0;
    }
    float3 _546;
    if (_545)
    {
        _546 = r.p - px0;
    }
    else
    {
        _546 = px1 - r.p;
    }
    float3 dx = _546;
    bool _578;
    if (y0)
    {
        bool _564 = !y1;
        bool _577;
        if (!_564)
        {
            _577 = distance(py0, r.p) < distance(py1, r.p);
        }
        else
        {
            _577 = _564;
        }
        _578 = _577;
    }
    else
    {
        _578 = y0;
    }
    float3 _579;
    if (_578)
    {
        _579 = r.p - py0;
    }
    else
    {
        _579 = py1 - r.p;
    }
    float3 dy = _579;
    float limit = max(0.0199999995529651641845703125f, length(view) * 0.0199999995529651641845703125f);
    bool _601 = length(dx) > limit;
    bool _609;
    if (!_601)
    {
        _609 = length(dy) > limit;
    }
    else
    {
        _609 = _601;
    }
    if (_609)
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
    bool _669 = !finite3(param_12);
    bool _678;
    if (!_669)
    {
        _678 = dot(ns, ns) < 0.25f;
    }
    else
    {
        _678 = _669;
    }
    if (_678)
    {
        return false;
    }
    r.ns = normalize(ns);
    bool _691 = dot(r.ns, r.v) <= 9.9999997473787516355514526367188e-06f;
    bool _701;
    if (!_691)
    {
        _701 = dot(r.ns, r.ng) <= 0.0f;
    }
    else
    {
        _701 = _691;
    }
    if (_701)
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

bool environmentNodeValid(GeometryNode node)
{
    float3 param = node.lo.xyz;
    bool _734 = finite3(param);
    bool _742;
    if (_734)
    {
        float3 param_1 = node.hi.xyz;
        _742 = finite3(param_1);
    }
    else
    {
        _742 = _734;
    }
    bool _753;
    if (_742)
    {
        _753 = all(bool3(node.lo.xyz.x <= node.hi.xyz.x, node.lo.xyz.y <= node.hi.xyz.y, node.lo.xyz.z <= node.hi.xyz.z));
    }
    else
    {
        _753 = _742;
    }
    return _753;
}

bool geometryBox(GeometryNode n, float3 o, float3 d, inout float nearT, inout float farT)
{
    for (uint k = 0u; k < 3u; k++)
    {
        if (d[k] == 0.0f)
        {
            bool _169 = o[k] < n.lo[k];
            bool _181;
            if (!_169)
            {
                _181 = o[k] > n.hi[k];
            }
            else
            {
                _181 = _169;
            }
            if (_181)
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
    precise float _82 = c * d;
    float cd = _82;
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
    bool _759 = !finite3(param);
    bool _767;
    if (!_759)
    {
        float3 param_1 = direction;
        _767 = !finite3(param_1);
    }
    else
    {
        _767 = _759;
    }
    bool _775;
    if (!_767)
    {
        float param_2 = nearT;
        _775 = !finite1(param_2);
    }
    else
    {
        _775 = _767;
    }
    bool _783;
    if (!_775)
    {
        float param_3 = farT;
        _783 = !finite1(param_3);
    }
    else
    {
        _783 = _775;
    }
    bool _790 = (_783 || (nearT < 0.0f)) || (farT <= nearT);
    bool _798;
    if (!_790)
    {
        _798 = dot(direction, direction) <= 0.0f;
    }
    else
    {
        _798 = _790;
    }
    if (_798)
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
        uint _818 = topVisited;
        topVisited = _818 + uint(1);
        uint _825;
        geometryTop.GetDimensions(_825);
        _825 = (_825 - 0) / 48;
        if (_818 >= uint(int(_825)))
        {
            return -1;
        }
        uint _833 = topSize;
        uint _834 = _833 - uint(1);
        topSize = _834;
        uint at = topStack[_834];
        uint _838;
        geometryTop.GetDimensions(_838);
        _838 = (_838 - 0) / 48;
        if (at >= uint(int(_838)))
        {
            return -1;
        }
        GeometryNode _849;
        _849.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _849.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _849.links = geometryTop.Load4(at * 48 + 32);
        node.lo = _849.lo;
        node.hi = _849.hi;
        node.links = _849.links;
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
        bool _874 = geometryBox(param_5, param_6, param_7, param_8, param_9);
        if (!_874)
        {
            continue;
        }
        if (node.links.w == 0u)
        {
            bool _886 = (topSize + 2u) > 64u;
            bool _896;
            if (!_886)
            {
                uint _892;
                geometryTop.GetDimensions(_892);
                _892 = (_892 - 0) / 48;
                _896 = node.links.x >= uint(int(_892));
            }
            else
            {
                _896 = _886;
            }
            bool _906;
            if (!_896)
            {
                uint _902;
                geometryTop.GetDimensions(_902);
                _902 = (_902 - 0) / 48;
                _906 = node.links.y >= uint(int(_902));
            }
            else
            {
                _906 = _896;
            }
            if (_906)
            {
                return -1;
            }
            uint _910 = topSize;
            topSize = _910 + uint(1);
            topStack[_910] = node.links.x;
            uint _915 = topSize;
            topSize = _915 + uint(1);
            topStack[_915] = node.links.y;
            continue;
        }
        bool _923 = node.links.w != 1u;
        bool _938;
        if (!_923)
        {
            uint _934;
            geometryInstances.GetDimensions(_934);
            _934 = (_934 - 0) / 64;
            _938 = node.links.z >= uint(int(_934));
        }
        else
        {
            _938 = _923;
        }
        if (_938)
        {
            return -1;
        }
        GeometryInstance _949;
        _949.inverse0 = asfloat(geometryInstances.Load4(node.links.z * 64 + 0));
        _949.inverse1 = asfloat(geometryInstances.Load4(node.links.z * 64 + 16));
        _949.inverse2 = asfloat(geometryInstances.Load4(node.links.z * 64 + 32));
        _949.identity = geometryInstances.Load4(node.links.z * 64 + 48);
        instance.inverse0 = _949.inverse0;
        instance.inverse1 = _949.inverse1;
        instance.inverse2 = _949.inverse2;
        instance.identity = _949.identity;
        bool _962 = any(isnan(instance.inverse0));
        bool _970;
        if (!_962)
        {
            _970 = any(isinf(instance.inverse0));
        }
        else
        {
            _970 = _962;
        }
        bool _978;
        if (!_970)
        {
            _978 = any(isnan(instance.inverse1));
        }
        else
        {
            _978 = _970;
        }
        bool _986;
        if (!_978)
        {
            _986 = any(isinf(instance.inverse1));
        }
        else
        {
            _986 = _978;
        }
        bool _994;
        if (!_986)
        {
            _994 = any(isnan(instance.inverse2));
        }
        else
        {
            _994 = _986;
        }
        bool _1002;
        if (!_994)
        {
            _1002 = any(isinf(instance.inverse2));
        }
        else
        {
            _1002 = _994;
        }
        if (_1002)
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
        bool _1078 = !finite3(param_11);
        bool _1086;
        if (!_1078)
        {
            float3 param_12 = localDirection;
            _1086 = !finite3(param_12);
        }
        else
        {
            _1086 = _1078;
        }
        bool _1094;
        if (!_1086)
        {
            float param_13 = directionSquared;
            _1094 = !finite1(param_13);
        }
        else
        {
            _1094 = _1086;
        }
        if (_1094 || (directionSquared <= 0.0f))
        {
            return -1;
        }
        uint bottomSize = 1u;
        uint bottomVisited = 0u;
        bottomStack[0] = instance.identity.x;
        while (bottomSize != 0u)
        {
            uint _1114 = bottomVisited;
            bottomVisited = _1114 + uint(1);
            uint _1120;
            geometryBottom.GetDimensions(_1120);
            _1120 = (_1120 - 0) / 48;
            if (_1114 >= uint(int(_1120)))
            {
                return -1;
            }
            uint _1127 = bottomSize;
            uint _1128 = _1127 - uint(1);
            bottomSize = _1128;
            at = bottomStack[_1128];
            uint _1132;
            geometryBottom.GetDimensions(_1132);
            _1132 = (_1132 - 0) / 48;
            if (at >= uint(int(_1132)))
            {
                return -1;
            }
            GeometryNode _1142;
            _1142.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _1142.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _1142.links = geometryBottom.Load4(at * 48 + 32);
            bottom.lo = _1142.lo;
            bottom.hi = _1142.hi;
            bottom.links = _1142.links;
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
            bool _1166 = geometryBox(param_15, param_16, param_17, param_18, param_19);
            if (!_1166)
            {
                continue;
            }
            if (bottom.links.w == 0u)
            {
                bool _1178 = (bottomSize + 2u) > 64u;
                bool _1188;
                if (!_1178)
                {
                    uint _1184;
                    geometryBottom.GetDimensions(_1184);
                    _1184 = (_1184 - 0) / 48;
                    _1188 = bottom.links.x >= uint(int(_1184));
                }
                else
                {
                    _1188 = _1178;
                }
                bool _1198;
                if (!_1188)
                {
                    uint _1194;
                    geometryBottom.GetDimensions(_1194);
                    _1194 = (_1194 - 0) / 48;
                    _1198 = bottom.links.y >= uint(int(_1194));
                }
                else
                {
                    _1198 = _1188;
                }
                if (_1198)
                {
                    return -1;
                }
                uint _1202 = bottomSize;
                bottomSize = _1202 + uint(1);
                bottomStack[_1202] = bottom.links.x;
                uint _1207 = bottomSize;
                bottomSize = _1207 + uint(1);
                bottomStack[_1207] = bottom.links.y;
                continue;
            }
            uint _1220;
            geometryTriangles.GetDimensions(_1220);
            _1220 = (_1220 - 0) / 48;
            bool _1223 = bottom.links.z > uint(int(_1220));
            bool _1236;
            if (!_1223)
            {
                uint _1229;
                geometryTriangles.GetDimensions(_1229);
                _1229 = (_1229 - 0) / 48;
                _1236 = bottom.links.w > (uint(int(_1229)) - bottom.links.z);
            }
            else
            {
                _1236 = _1223;
            }
            if (_1236)
            {
                return -1;
            }
            for (uint i = 0u; i < bottom.links.w; i++)
            {
                GeometryTriangle _1259;
                _1259.a = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 0));
                _1259.e1 = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 16));
                _1259.e2 = asfloat(geometryTriangles.Load4((bottom.links.z + i) * 48 + 32));
                _triangle.a = _1259.a;
                _triangle.e1 = _1259.e1;
                _triangle.e2 = _1259.e2;
                float3 param_20 = _triangle.a.xyz;
                bool _1271 = !finite3(param_20);
                bool _1281;
                if (!_1271)
                {
                    float3 param_21 = _triangle.e1.xyz;
                    _1281 = !finite3(param_21);
                }
                else
                {
                    _1281 = _1271;
                }
                bool _1291;
                if (!_1281)
                {
                    float3 param_22 = _triangle.e2.xyz;
                    _1291 = !finite3(param_22);
                }
                else
                {
                    _1291 = _1281;
                }
                if (_1291)
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
                bool _1366 = v < 0.0f;
                bool _1374;
                if (!_1366)
                {
                    _1374 = (u + v) > 1.0f;
                }
                else
                {
                    _1374 = _1366;
                }
                if (_1374)
                {
                    continue;
                }
                float3 param_26 = _triangle.e1.xyz;
                float3 param_27 = _triangle.e2.xyz;
                float3 normal = geometryTriangleNormal(param_26, param_27);
                float planeDet = dot(localDirection, normal);
                float3 param_28 = normal;
                bool _1395 = !finite3(param_28);
                bool _1403;
                if (!_1395)
                {
                    float param_29 = planeDet;
                    _1403 = !finite1(param_29);
                }
                else
                {
                    _1403 = _1395;
                }
                if (_1403 || (planeDet == 0.0f))
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
                bool _1441 = t <= (nearT + 1.9999999494757503271102905273438e-05f);
                bool _1449;
                if (!_1441)
                {
                    _1449 = t >= (farT - 1.9999999494757503271102905273438e-05f);
                }
                else
                {
                    _1449 = _1441;
                }
                if (_1449)
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
    bool _1477 = cb_policy.x != 1u;
    bool _1484;
    if (!_1477)
    {
        uint _1481;
        geometryTop.GetDimensions(_1481);
        _1481 = (_1481 - 0) / 48;
        _1484 = int(_1481) == 0;
    }
    else
    {
        _1484 = _1477;
    }
    bool _1491;
    if (!_1484)
    {
        uint _1488;
        geometryBottom.GetDimensions(_1488);
        _1488 = (_1488 - 0) / 48;
        _1491 = int(_1488) == 0;
    }
    else
    {
        _1491 = _1484;
    }
    bool _1498;
    if (!_1491)
    {
        uint _1495;
        geometryTriangles.GetDimensions(_1495);
        _1495 = (_1495 - 0) / 48;
        _1498 = int(_1495) == 0;
    }
    else
    {
        _1498 = _1491;
    }
    bool _1505;
    if (!_1498)
    {
        uint _1502;
        geometryInstances.GetDimensions(_1502);
        _1502 = (_1502 - 0) / 64;
        _1505 = int(_1502) == 0;
    }
    else
    {
        _1505 = _1498;
    }
    if (_1505)
    {
        return 0;
    }
    GeometryNode _1511;
    _1511.lo = asfloat(geometryTop.Load4(0));
    _1511.hi = asfloat(geometryTop.Load4(16));
    _1511.links = geometryTop.Load4(32);
    GeometryNode root;
    root.lo = _1511.lo;
    root.hi = _1511.hi;
    root.links = _1511.links;
    float3 param_1 = root.lo.xyz;
    bool _1523 = !finite3(param_1);
    bool _1533;
    if (!_1523)
    {
        float3 param_2 = root.hi.xyz;
        _1533 = !finite3(param_2);
    }
    else
    {
        _1533 = _1523;
    }
    bool _1545;
    if (!_1533)
    {
        _1545 = any(bool3(root.lo.xyz.x > root.hi.xyz.x, root.lo.xyz.y > root.hi.xyz.y, root.lo.xyz.z > root.hi.xyz.z));
    }
    else
    {
        _1545 = _1533;
    }
    if (_1545)
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
            bool _1581 = origin[k] < root.lo[k];
            bool _1592;
            if (!_1581)
            {
                _1592 = origin[k] > root.hi[k];
            }
            else
            {
                _1592 = _1581;
            }
            if (_1592)
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
    int _1671;
    if (hit < 0)
    {
        _1671 = 0;
    }
    else
    {
        _1671 = (hit > 0) ? 1 : 2;
    }
    return _1671;
}

float4 mirrorFallback(Receiver r)
{
    if (cb_policy.y == 1u)
    {
        return float4(1.0f, 0.0f, 2.0f, 1.0f);
    }
    if (cb_policy.x != 1u)
    {
        return float4(0.0f, 0.0f, 3.0f, 1.0f);
    }
    Receiver param = r;
    float3 param_1 = normalize(reflect(-r.v, r.ns));
    int result = environmentRay(param, param_1);
    return float4(float(result == 2), float(result != 0), (result != 0) ? 2.0f : 3.0f, 1.0f);
}

void comp_main()
{
    int2 pixel = int2(gl_GlobalInvocationID.xy);
    int2 cacheOrigin = (int2(gl_WorkGroupID.xy) * int2(4, 4)) - int2(1, 1);
    uint lane = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    if (lane < 36u)
    {
        int2 halfPixel = cacheOrigin + int2(int(lane % 6u), int(lane / 6u));
        bool _1765 = all(bool2(halfPixel.x >= int2(0, 0).x, halfPixel.y >= int2(0, 0).y));
        bool _1775;
        if (_1765)
        {
            int2 _1772 = int2(cb_dimensions.zw);
            _1775 = all(bool2(halfPixel.x < _1772.x, halfPixel.y < _1772.y));
        }
        else
        {
            _1775 = _1765;
        }
        bool valid = _1775;
        Receiver cached;
        if (valid)
        {
            int2 samplePixel = min(((halfPixel * int2(2, 2)) + int2(1, 1)), (int2(cb_dimensions.xy) - int2(1, 1)));
            int2 param = samplePixel;
            Receiver param_1;
            bool _1795 = receiver(param, param_1);
            cached = param_1;
            valid = _1795;
        }
        if (valid)
        {
            sourceReceiverCache[lane] = cached;
        }
        sourceReceiverValid[lane] = uint(valid);
    }
    GroupMemoryBarrierWithGroupSync();
    int2 _1820 = int2(cb_dimensions.xy);
    if (any(bool2(pixel.x >= _1820.x, pixel.y >= _1820.y)))
    {
        return;
    }
    int2 param_2 = pixel;
    Receiver param_3;
    bool _1830 = receiver(param_2, param_3);
    Receiver target = param_3;
    if (!_1830)
    {
        outputVisibility[pixel] = float4(0.0f, 0.0f, 1.0f, 0.0f);
        return;
    }
    float best = 3.4028234663852885981170418348452e+38f;
    float4 result = 0.0f.xxxx;
    bool found = false;
    int2 base = max(((pixel - int2(1, 1)) / int2(2, 2)), int2(0, 0));
    for (int y = 0; y <= 1; y++)
    {
        for (int x = 0; x <= 1; x++)
        {
            int2 h = base + int2(x, y);
            bool _1876 = any(bool2(h.x < int2(0, 0).x, h.y < int2(0, 0).y));
            bool _1887;
            if (!_1876)
            {
                int2 _1884 = int2(cb_dimensions.zw);
                _1887 = any(bool2(h.x >= _1884.x, h.y >= _1884.y));
            }
            else
            {
                _1887 = _1876;
            }
            if (_1887)
            {
                continue;
            }
            int2 cachePixel = h - cacheOrigin;
            uint cacheIndex = uint((cachePixel.y * 6) + cachePixel.x);
            if (sourceReceiverValid[cacheIndex] == 0u)
            {
                continue;
            }
            Receiver source = sourceReceiverCache[cacheIndex];
            float3 delta = source.p - target.p;
            float nsGate = lerp(0.999499976634979248046875f, 0.89999997615814208984375f, target.roughness * target.roughness);
            bool _1937 = dot(source.ng, target.ng) < cb_parameters.z;
            bool _1948;
            if (!_1937)
            {
                _1948 = dot(source.ns, target.ns) < nsGate;
            }
            else
            {
                _1948 = _1937;
            }
            bool _1961;
            if (!_1948)
            {
                _1961 = abs(source.roughness - target.roughness) > cb_parameters.w;
            }
            else
            {
                _1961 = _1948;
            }
            bool _1973;
            if (!_1961)
            {
                _1973 = abs(dot(delta, target.ng)) > cb_parameters.y;
            }
            else
            {
                _1973 = _1961;
            }
            bool _1985;
            if (!_1973)
            {
                _1985 = abs(dot(delta, source.ng)) > cb_parameters.y;
            }
            else
            {
                _1985 = _1973;
            }
            if (_1985)
            {
                continue;
            }
            float key = dot(delta, delta);
            if (key > max(0.0024999999441206455230712890625f, dot(cb_camera.xyz - target.p, cb_camera.xyz - target.p) * 2.4999999368446879088878631591797e-05f))
            {
                continue;
            }
            float4 value = halfVisibility.Load(int3(h, 0));
            bool _2022 = any(isnan(value));
            bool _2029;
            if (!_2022)
            {
                _2029 = any(isinf(value));
            }
            else
            {
                _2029 = _2022;
            }
            bool _2036;
            if (!_2029)
            {
                _2036 = value.w < 0.5f;
            }
            else
            {
                _2036 = _2029;
            }
            if (_2036)
            {
                continue;
            }
            if (key < best)
            {
                best = key;
                result = value;
                found = true;
            }
        }
    }
    float4 _2054;
    if (found)
    {
        _2054 = result;
    }
    else
    {
        Receiver param_4 = target;
        _2054 = mirrorFallback(param_4);
    }
    outputVisibility[pixel] = _2054;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
