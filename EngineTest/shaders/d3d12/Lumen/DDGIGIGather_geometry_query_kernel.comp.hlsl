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

struct TGProbe
{
    float3 sh[9];
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

struct GPUStaticProbeData
{
    float4 gridOrigin;
    uint3 gridCounts;
    float _pad0;
    float4 skySH[9];
};

static const float3 _2766[4] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f) };

ByteAddressBuffer geometryTop : register(t23, space0);
ByteAddressBuffer geometryInstances : register(t26, space0);
ByteAddressBuffer geometryBottom : register(t24, space0);
ByteAddressBuffer geometryTriangles : register(t25, space0);
cbuffer ConnectionPolicy : register(b27, space0)
{
    uint4 _1262_connectionPolicy : packoffset(c0);
    float4 _1262_connectionJitter : packoffset(c1);
};

cbuffer ViewCB : register(b4, space0)
{
    float4 _1275_viewRows[4] : packoffset(c0);
};

cbuffer ProbeCounts : register(b6, space0)
{
    float4 _1393_probeCounts : packoffset(c0);
    float4 _1393_cameraPos : packoffset(c1);
    float4 _1393_debugVis : packoffset(c2);
};

ByteAddressBuffer _2054 : register(t12, space0);
cbuffer ProbeOriginSpacing : register(b5, space0)
{
    float4 _2376_probeOriginSpacing : packoffset(c0);
};

ByteAddressBuffer _2746 : register(t11, space0);
ByteAddressBuffer _3284 : register(t8, space0);
ByteAddressBuffer _3705 : register(t10, space0);
cbuffer FlatVolumeCB : register(b18, space0)
{
    float4 _3739_fvOriginSpacing[4] : packoffset(c0);
    float4 _3739_fvCountsOffset[4] : packoffset(c4);
    float4 _3739_fvMeta : packoffset(c8);
};

ByteAddressBuffer _3987 : register(t17, space0);
ByteAddressBuffer _4390 : register(t20, space0);
ByteAddressBuffer _4471 : register(t19, space0);
ByteAddressBuffer _5127 : register(t7, space0);
cbuffer StaticProbeCB : register(b9, space0)
{
    GPUStaticProbeData _5132_staticProbe : packoffset(c0);
};

Texture2D<float4> gbuffer_depth : register(t0, space0);
RWTexture2D<float4> output_tex : register(u2, space0);
Texture2D<float4> gbuffer_normal : register(t1, space0);
RWTexture2D<float4> spec_output_tex : register(u14, space0);
Texture2D<float4> gbuffer_orm : register(t13, space0);
Texture2D<float4> history_tex : register(t3, space0);

static uint3 gl_LocalInvocationID;
static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

groupshared bool tg_cellFound;
groupshared uint3 tg_cellMin;
groupshared uint tg_probe_idx[8];
groupshared uint tg_probe_depth_base[8];
groupshared TGProbe tg_probes[8];
groupshared float tg_skyFactor[8];
groupshared float tg_depth_map[1024];
groupshared float3 tg_indirect[64];
groupshared float tg_pixelDepth[64];

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

uint2 spvImageSize(RWTexture2D<float4> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

bool connectionSurfacePoint(int2 pixel, inout float3 p)
{
    uint _1200_dummy_parameter;
    int2 size = int2(spvTextureSize(gbuffer_depth, uint(0), _1200_dummy_parameter));
    bool _1205 = any(bool2(pixel.x < int2(0, 0).x, pixel.y < int2(0, 0).y));
    bool _1213;
    if (!_1205)
    {
        _1213 = any(bool2(pixel.x >= size.x, pixel.y >= size.y));
    }
    else
    {
        _1213 = _1205;
    }
    if (_1213)
    {
        return false;
    }
    float z = gbuffer_depth.Load(int3(pixel, 0)).x;
    bool _1223 = isnan(z);
    bool _1229;
    if (!_1223)
    {
        _1229 = isinf(z);
    }
    else
    {
        _1229 = _1223;
    }
    if ((_1229 || (z < 0.0f)) || (z >= 0.999998986721038818359375f))
    {
        return false;
    }
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(size);
    float2 ndc = float2((2.0f * uv.x) - 1.0f, 1.0f - (2.0f * uv.y)) - _1262_connectionJitter.xy;
    float4x4 invVP = float4x4(float4(_1275_viewRows[0]), float4(_1275_viewRows[1]), float4(_1275_viewRows[2]), float4(_1275_viewRows[3]));
    float4 h = mul(float4(ndc, z, 1.0f), invVP);
    bool _1316 = any(isnan(h));
    bool _1323;
    if (!_1316)
    {
        _1323 = any(isinf(h));
    }
    else
    {
        _1323 = _1316;
    }
    bool _1332;
    if (!_1323)
    {
        _1332 = abs(h.w) < 9.9999999392252902907785028219223e-09f;
    }
    else
    {
        _1332 = _1323;
    }
    if (_1332)
    {
        return false;
    }
    p = h.xyz / h.w.xxx;
    bool _1345 = !any(isnan(p));
    bool _1352;
    if (_1345)
    {
        _1352 = !any(isinf(p));
    }
    else
    {
        _1352 = _1345;
    }
    return _1352;
}

bool connectionSurfaceOrigin(int2 pixel, inout float3 origin)
{
    bool _1358 = _1262_connectionPolicy.x != 1u;
    bool _1367;
    if (!_1358)
    {
        _1367 = any(isnan(_1262_connectionJitter.xy));
    }
    else
    {
        _1367 = _1358;
    }
    bool _1376;
    if (!_1367)
    {
        _1376 = any(isinf(_1262_connectionJitter.xy));
    }
    else
    {
        _1376 = _1367;
    }
    if (_1376)
    {
        return false;
    }
    int2 param = pixel;
    float3 param_1;
    bool _1384 = connectionSurfacePoint(param, param_1);
    float3 p = param_1;
    if (!_1384)
    {
        return false;
    }
    float3 v = _1393_cameraPos.xyz - p;
    bool _1401 = any(isnan(v));
    bool _1408;
    if (!_1401)
    {
        _1408 = any(isinf(v));
    }
    else
    {
        _1408 = _1401;
    }
    bool _1417;
    if (!_1408)
    {
        _1417 = dot(v, v) < 1.0000000133514319600180897396058e-10f;
    }
    else
    {
        _1417 = _1408;
    }
    if (_1417)
    {
        return false;
    }
    int2 param_2 = pixel - int2(1, 0);
    float3 param_3;
    bool _1428 = connectionSurfacePoint(param_2, param_3);
    float3 a = param_3;
    bool ax = _1428;
    int2 param_4 = pixel + int2(1, 0);
    float3 param_5;
    bool _1436 = connectionSurfacePoint(param_4, param_5);
    float3 b = param_5;
    bool bx = _1436;
    int2 param_6 = pixel - int2(0, 1);
    float3 param_7;
    bool _1445 = connectionSurfacePoint(param_6, param_7);
    float3 c = param_7;
    bool cy = _1445;
    int2 param_8 = pixel + int2(0, 1);
    float3 param_9;
    bool _1453 = connectionSurfacePoint(param_8, param_9);
    float3 d = param_9;
    bool dy = _1453;
    bool _1459 = (!ax) && (!bx);
    bool _1468;
    if (!_1459)
    {
        _1468 = (!cy) && (!dy);
    }
    else
    {
        _1468 = _1459;
    }
    if (_1468)
    {
        return false;
    }
    bool _1489;
    if (ax)
    {
        bool _1477 = !bx;
        bool _1488;
        if (!_1477)
        {
            _1488 = distance(a, p) < distance(b, p);
        }
        else
        {
            _1488 = _1477;
        }
        _1489 = _1488;
    }
    else
    {
        _1489 = ax;
    }
    float3 _1490;
    if (_1489)
    {
        _1490 = p - a;
    }
    else
    {
        _1490 = b - p;
    }
    float3 dx = _1490;
    bool _1518;
    if (cy)
    {
        bool _1506 = !dy;
        bool _1517;
        if (!_1506)
        {
            _1517 = distance(c, p) < distance(d, p);
        }
        else
        {
            _1517 = _1506;
        }
        _1518 = _1517;
    }
    else
    {
        _1518 = cy;
    }
    float3 _1519;
    if (_1518)
    {
        _1519 = p - c;
    }
    else
    {
        _1519 = d - p;
    }
    float3 ddy = _1519;
    float limit = max(0.0199999995529651641845703125f, length(v) * 0.0199999995529651641845703125f);
    bool _1539 = length(dx) > limit;
    bool _1547;
    if (!_1539)
    {
        _1547 = length(ddy) > limit;
    }
    else
    {
        _1547 = _1539;
    }
    if (_1547)
    {
        return false;
    }
    float3 crossN = cross(dx, ddy);
    float scale = max(abs(crossN.x), max(abs(crossN.y), abs(crossN.z)));
    bool _1569 = any(isnan(crossN));
    bool _1576;
    if (!_1569)
    {
        _1576 = any(isinf(crossN));
    }
    else
    {
        _1576 = _1569;
    }
    if (_1576 || (scale < 1.000000016862383526387164645044e-16f))
    {
        return false;
    }
    float3 ng = normalize(crossN / scale.xxx);
    if (dot(ng, v) < 0.0f)
    {
        ng = -ng;
    }
    origin = p + (ng * 0.00200000009499490261077880859375f);
    bool _1606 = !any(isnan(origin));
    bool _1613;
    if (_1606)
    {
        _1613 = !any(isinf(origin));
    }
    else
    {
        _1613 = _1606;
    }
    return _1613;
}

float2 gatherOctEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 uv = d.xy / l1norm.xx;
    if (d.z < 0.0f)
    {
        bool2 _2010 = bool2(uv.x >= 0.0f.xx.x, uv.y >= 0.0f.xx.y);
        uv = (1.0f.xx - abs(uv.yx)) * float2(_2010.x ? 1.0f.xx.x : (-1.0f).xx.x, _2010.y ? 1.0f.xx.y : (-1.0f).xx.y);
    }
    return (uv * 0.5f) + 0.5f.xx;
}

float2 sampleNearProbeDepth(uint probeIdx, float3 dir)
{
    float3 param = dir;
    float2 uv = gatherOctEncode(param);
    uint2 texel = uint2(clamp(uint(uv.x * 8.0f), 0u, 7u), clamp(uint(uv.y * 8.0f), 0u, 7u));
    uint octIdx = (texel.y * 8u) + texel.x;
    uint base = probeIdx * 128u;
    return float2(asfloat(_2054.Load((base + octIdx) * 4 + 0)), asfloat(_2054.Load(((base + 64u) + octIdx) * 4 + 0)));
}

float3 visProbePositions(float2 uv)
{
    float4x4 invVP = float4x4(float4(_1275_viewRows[0]), float4(_1275_viewRows[1]), float4(_1275_viewRows[2]), float4(_1275_viewRows[3]));
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 farH = mul(float4(ndc, 0.999000012874603271484375f, 1.0f), invVP);
    float3 ro = _1393_cameraPos.xyz;
    float3 rd = (farH.xyz / farH.w.xxx) - ro;
    float rdLen = length(rd);
    if (rdLen < 9.9999997473787516355514526367188e-06f)
    {
        return float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
    }
    rd /= rdLen.xxx;
    float3 vorg = _2376_probeOriginSpacing.xyz;
    float sp = max(_2376_probeOriginSpacing.w, 0.00999999977648258209228515625f);
    float3 counts = _1393_probeCounts.xyz;
    float rad = 0.4000000059604644775390625f * sp;
    float3 bmin = vorg - rad.xxx;
    float3 bmax = (vorg + ((counts - 1.0f.xxx) * sp)) + rad.xxx;
    float3 inv = 1.0f.xxx / rd;
    float3 t0s = (bmin - ro) * inv;
    float3 t1s = (bmax - ro) * inv;
    float3 tsm = min(t0s, t1s);
    float3 tbg = max(t0s, t1s);
    float tEnter = max(max(tsm.x, tsm.y), tsm.z);
    float tExit = min(min(min(tbg.x, tbg.y), tbg.z), rdLen);
    if (tExit < max(tEnter, 0.0f))
    {
        return float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
    }
    float t = max(tEnter, 0.0f) + 9.9999997473787516355514526367188e-05f;
    int3 cell = clamp(int3(floor(((ro + (rd * t)) - vorg) / sp.xxx)), int3(0, 0, 0), int3(counts) - int3(1, 1, 1));
    int3 stp = int3(sign(rd));
    float3 tDelta = abs(inv) * sp;
    float3 nextB = vorg + ((float3(cell) + max(float3(stp), 0.0f.xxx)) * sp);
    float3 tMax = (nextB - ro) * inv;
    for (int a = 0; a < 3; a++)
    {
        if (stp[a] == 0)
        {
            tMax[a] = 1000000015047466219876688855040.0f;
        }
    }
    float bestT = -1.0f;
    int3 bestCell = int3(0, 0, 0);
    for (int it = 0; it < 96; it++)
    {
        bool _2543 = any(bool3(cell.x < int3(0, 0, 0).x, cell.y < int3(0, 0, 0).y, cell.z < int3(0, 0, 0).z));
        bool _2552;
        if (!_2543)
        {
            float3 _2548 = float3(cell);
            _2552 = any(bool3(_2548.x >= counts.x, _2548.y >= counts.y, _2548.z >= counts.z));
        }
        else
        {
            _2552 = _2543;
        }
        if (_2552)
        {
            break;
        }
        float3 center = vorg + (float3(cell) * sp);
        float3 oc = ro - center;
        float b = dot(oc, rd);
        float c = dot(oc, oc) - (rad * rad);
        float disc = (b * b) - c;
        if (disc > 0.0f)
        {
            float th = (-b) - sqrt(disc);
            bool _2600 = (th > 0.0f) && (th <= tExit);
            bool _2609;
            if (_2600)
            {
                _2609 = (bestT < 0.0f) || (th < bestT);
            }
            else
            {
                _2609 = _2600;
            }
            if (_2609)
            {
                bestT = th;
                bestCell = cell;
            }
        }
        if (tMax.x < tMax.y)
        {
            if (tMax.x < tMax.z)
            {
                t = tMax.x;
                cell.x += stp.x;
                tMax.x += tDelta.x;
            }
            else
            {
                t = tMax.z;
                cell.z += stp.z;
                tMax.z += tDelta.z;
            }
        }
        else
        {
            if (tMax.y < tMax.z)
            {
                t = tMax.y;
                cell.y += stp.y;
                tMax.y += tDelta.y;
            }
            else
            {
                t = tMax.z;
                cell.z += stp.z;
                tMax.z += tDelta.z;
            }
        }
        bool _2696 = t > tExit;
        bool _2706;
        if (!_2696)
        {
            _2706 = (bestT > 0.0f) && (t > bestT);
        }
        else
        {
            _2706 = _2696;
        }
        if (_2706)
        {
            break;
        }
    }
    if (bestT < 0.0f)
    {
        return float3(0.100000001490116119384765625f, 0.0199999995529651641845703125f, 0.100000001490116119384765625f);
    }
    uint pIdx = (uint(bestCell.x) + (uint(bestCell.y) * uint(counts.x))) + ((uint(bestCell.z) * uint(counts.x)) * uint(counts.y));
    float3 L0 = asfloat(_2746.Load4((pIdx * 9u) * 16 + 0)).xyz;
    float lum = dot(L0, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    float meanD = 0.0f;
    for (int k = 0; k < 4; k++)
    {
        uint param = pIdx;
        float3 param_1 = _2766[k];
        meanD += sampleNearProbeDepth(param, param_1).x;
    }
    meanD *= 0.25f;
    bool buried = meanD < (0.25f * sp);
    float e = clamp(log2(1.0f + lum) / 6.0f, 0.0f, 1.0f);
    float3 heat = lerp(float3(0.0199999995529651641845703125f, 0.07999999821186065673828125f, 0.20000000298023223876953125f), float3(0.100000001490116119384765625f, 0.85000002384185791015625f, 0.3499999940395355224609375f), e.xxx);
    heat = lerp(heat, float3(1.0f, 0.949999988079071044921875f, 0.449999988079071044921875f), smoothstep(0.550000011920928955078125f, 1.0f, e).xxx);
    if (buried)
    {
        heat = lerp(heat, float3(1.0f, 0.119999997317790985107421875f, 0.07999999821186065673828125f), 0.85000002384185791015625f.xxx);
    }
    float3 n = ((ro + (rd * bestT)) - (vorg + (float3(bestCell) * sp))) / rad.xxx;
    float shade = 0.3499999940395355224609375f + (0.64999997615814208984375f * max(dot(n, float3(0.4239991605281829833984375f, 0.847998321056365966796875f, 0.317999362945556640625f)), 0.0f));
    return (heat * shade) * 6.0f;
}

float3 evalProbeIrradianceSH9(float3 sh[9], float3 n)
{
    float x = n.x;
    float y = n.y;
    float z = n.z;
    float xy = x * y;
    float yz = y * z;
    float zx = z * x;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    return ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.325735032558441162109375f) * y))) + (sh[2] * (0.325735032558441162109375f * z))) + (sh[3] * ((-0.325735032558441162109375f) * x))) + (sh[4] * (0.2731371223926544189453125f * xy))) + (sh[5] * ((-0.2731371223926544189453125f) * yz))) + (sh[6] * (0.078847892582416534423828125f * ((3.0f * zz) - 1.0f)))) + (sh[7] * ((-0.2731371223926544189453125f) * zx))) + (sh[8] * (0.136568546295166015625f * (xx - yy)));
}

float measuredConnectionVisibility(float _distance, float2 mv)
{
    bool _2211 = any(isnan(mv));
    bool _2218;
    if (!_2211)
    {
        _2218 = any(isinf(mv));
    }
    else
    {
        _2218 = _2211;
    }
    bool _2225;
    if (!_2218)
    {
        _2225 = mv.x < 0.0f;
    }
    else
    {
        _2225 = _2218;
    }
    bool _2232;
    if (!_2225)
    {
        _2232 = mv.y < 0.0f;
    }
    else
    {
        _2232 = _2225;
    }
    bool _2238;
    if (!_2232)
    {
        _2238 = isnan(_distance);
    }
    else
    {
        _2238 = _2232;
    }
    bool _2244;
    if (!_2238)
    {
        _2244 = isinf(_distance);
    }
    else
    {
        _2244 = _2238;
    }
    if (_2244)
    {
        return 0.0f;
    }
    float beyond = max(_distance - mv.x, 0.0f);
    if (beyond == 0.0f)
    {
        return 1.0f;
    }
    return mv.y / (mv.y + (beyond * beyond));
}

float visibilityWeight(float distToProbe, float mean, float variance, float spacing)
{
    float dist = max(distToProbe - mean, 0.0f);
    if (variance < 9.9999997473787516355514526367188e-05f)
    {
        return 1.0f;
    }
    float cheb = variance / (variance + (dist * dist));
    return clamp(max(cheb * 2.0f, 0.20000000298023223876953125f), 0.0f, 1.0f);
}

bool certifiedFloorBlock(float3 probe, float3 receiver, float3 dir, float _distance, float2 mv)
{
    bool _2072 = _1393_debugVis.y <= 0.0f;
    bool _2079;
    if (!_2072)
    {
        _2079 = _1393_debugVis.z <= 0.0f;
    }
    else
    {
        _2079 = _2072;
    }
    if (_2079)
    {
        return false;
    }
    float3 param = dir;
    float2 uv = gatherOctEncode(param);
    uint2 cell = uint2(clamp(uv * 8.0f, 0.0f.xx, 7.0f.xx));
    uint bin = (cell.y * 8u) + cell.x;
    uint radianceMask = (bin < 32u) ? 2160091943u : 3697868u;
    if ((radianceMask & (1u << (bin % 32u))) != 0u)
    {
        return false;
    }
    float plane = _1393_debugVis.w;
    bool _2125 = probe.y >= (plane - 0.100000001490116119384765625f);
    bool _2135;
    if (!_2125)
    {
        _2135 = receiver.y <= (plane + 0.00999999977648258209228515625f);
    }
    else
    {
        _2135 = _2125;
    }
    if (_2135)
    {
        return false;
    }
    bool _2141 = mv.x <= 0.0f;
    bool _2148;
    if (!_2141)
    {
        _2148 = mv.y <= 0.0f;
    }
    else
    {
        _2148 = _2141;
    }
    bool _2163;
    if (!_2148)
    {
        _2163 = (mv.x + (2.0f * sqrt(mv.y))) >= (_distance - 0.1500000059604644775390625f);
    }
    else
    {
        _2163 = _2148;
    }
    if (_2163)
    {
        return false;
    }
    float t = (plane - probe.y) / (receiver.y - probe.y);
    float3 crossing = lerp(probe, receiver, t.xxx);
    bool _2188 = (t > 0.0f) && (t < 1.0f);
    bool _2197;
    if (_2188)
    {
        _2197 = abs(crossing.x) < _1393_debugVis.y;
    }
    else
    {
        _2197 = _2188;
    }
    bool _2206;
    if (_2197)
    {
        _2206 = abs(crossing.z) < _1393_debugVis.z;
    }
    else
    {
        _2206 = _2197;
    }
    return _2206;
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
            bool _403 = o[k] < n.lo[k];
            bool _414;
            if (!_403)
            {
                _414 = o[k] > n.hi[k];
            }
            else
            {
                _414 = _403;
            }
            if (_414)
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
    precise float _321 = c * d;
    float cd = _321;
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

int geometryFirstHit(float3 o, float3 d, float nearT, float farT, inout float bestT, out uint owner, out uint bestPrimitive)
{
    bool _526;
    if (false)
    {
        _526 = all(bool3(d.x != 0.0f.xxx.x, d.y != 0.0f.xxx.y, d.z != 0.0f.xxx.z));
    }
    else
    {
        _526 = false;
    }
    bool _533;
    if (_526)
    {
        _533 = !any(isnan(d));
    }
    else
    {
        _533 = _526;
    }
    bool _540;
    if (_533)
    {
        _540 = !any(isinf(d));
    }
    else
    {
        _540 = _533;
    }
    bool topNonzero = _540;
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
        uint _558 = tp;
        uint _559 = _558 - uint(1);
        tp = _559;
        uint at = ts[_559];
        uint _568;
        geometryTop.GetDimensions(_568);
        _568 = (_568 - 0) / 48;
        if (at >= uint(int(_568)))
        {
            return -1;
        }
        GeometryNode _580;
        _580.lo = asfloat(geometryTop.Load4(at * 48 + 0));
        _580.hi = asfloat(geometryTop.Load4(at * 48 + 16));
        _580.links = geometryTop.Load4(at * 48 + 32);
        n.lo = _580.lo;
        n.hi = _580.hi;
        n.links = _580.links;
        if (topNonzero)
        {
            GeometryNode param = n;
            float3 param_1 = o;
            float3 param_2 = d;
            float param_3 = nearT;
            float param_4 = bestT;
            bool _603 = geometryBoxNonzero(param, param_1, param_2, param_3, param_4);
            topHit = _603;
        }
        else
        {
            GeometryNode param_5 = n;
            float3 param_6 = o;
            float3 param_7 = d;
            float param_8 = nearT;
            float param_9 = bestT;
            bool _615 = geometryBox(param_5, param_6, param_7, param_8, param_9);
            topHit = _615;
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
            uint _632 = tp;
            tp = _632 + uint(1);
            ts[_632] = n.links.x;
            uint _637 = tp;
            tp = _637 + uint(1);
            ts[_637] = n.links.y;
            continue;
        }
        uint _650;
        geometryInstances.GetDimensions(_650);
        _650 = (_650 - 0) / 64;
        if (n.links.z >= uint(int(_650)))
        {
            return -1;
        }
        GeometryInstance _664;
        _664.inverse0 = asfloat(geometryInstances.Load4(n.links.z * 64 + 0));
        _664.inverse1 = asfloat(geometryInstances.Load4(n.links.z * 64 + 16));
        _664.inverse2 = asfloat(geometryInstances.Load4(n.links.z * 64 + 32));
        _664.identity = geometryInstances.Load4(n.links.z * 64 + 48);
        inst.inverse0 = _664.inverse0;
        inst.inverse1 = _664.inverse1;
        inst.inverse2 = _664.inverse2;
        inst.identity = _664.identity;
        float3 lo = float3(dot(inst.inverse0, float4(o, 1.0f)), dot(inst.inverse1, float4(o, 1.0f)), dot(inst.inverse2, float4(o, 1.0f)));
        float3 ld = float3(dot(inst.inverse0.xyz, d), dot(inst.inverse1.xyz, d), dot(inst.inverse2.xyz, d));
        bool _718 = any(isnan(lo));
        bool _725;
        if (!_718)
        {
            _725 = any(isinf(lo));
        }
        else
        {
            _725 = _718;
        }
        bool _732;
        if (!_725)
        {
            _732 = any(isnan(ld));
        }
        else
        {
            _732 = _725;
        }
        bool _739;
        if (!_732)
        {
            _739 = any(isinf(ld));
        }
        else
        {
            _739 = _732;
        }
        if (_739)
        {
            return -1;
        }
        bool _749;
        if (false)
        {
            _749 = all(bool3(ld.x != 0.0f.xxx.x, ld.y != 0.0f.xxx.y, ld.z != 0.0f.xxx.z));
        }
        else
        {
            _749 = false;
        }
        bool bottomNonzero = _749;
        uint bp = 1u;
        bs[0] = inst.identity.x;
        while (bp != 0u)
        {
            uint _762 = bp;
            uint _763 = _762 - uint(1);
            bp = _763;
            at = bs[_763];
            uint _771;
            geometryBottom.GetDimensions(_771);
            _771 = (_771 - 0) / 48;
            if (at >= uint(int(_771)))
            {
                return -1;
            }
            GeometryNode _781;
            _781.lo = asfloat(geometryBottom.Load4(at * 48 + 0));
            _781.hi = asfloat(geometryBottom.Load4(at * 48 + 16));
            _781.links = geometryBottom.Load4(at * 48 + 32);
            b.lo = _781.lo;
            b.hi = _781.hi;
            b.links = _781.links;
            if (bottomNonzero)
            {
                GeometryNode param_10 = b;
                float3 param_11 = lo;
                float3 param_12 = ld;
                float param_13 = nearT;
                float param_14 = bestT;
                bool _802 = geometryBoxNonzero(param_10, param_11, param_12, param_13, param_14);
                bottomHit = _802;
            }
            else
            {
                GeometryNode param_15 = b;
                float3 param_16 = lo;
                float3 param_17 = ld;
                float param_18 = nearT;
                float param_19 = bestT;
                bool _814 = geometryBox(param_15, param_16, param_17, param_18, param_19);
                bottomHit = _814;
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
                uint _831 = bp;
                bp = _831 + uint(1);
                bs[_831] = b.links.x;
                uint _836 = bp;
                bp = _836 + uint(1);
                bs[_836] = b.links.y;
                continue;
            }
            uint _849;
            geometryTriangles.GetDimensions(_849);
            _849 = (_849 - 0) / 48;
            bool _852 = b.links.z > uint(int(_849));
            bool _865;
            if (!_852)
            {
                uint _858;
                geometryTriangles.GetDimensions(_858);
                _858 = (_858 - 0) / 48;
                _865 = b.links.w > (uint(int(_858)) - b.links.z);
            }
            else
            {
                _865 = _852;
            }
            if (_865)
            {
                return -1;
            }
            for (uint i = 0u; i < b.links.w; i++)
            {
                GeometryTriangle _888;
                _888.a = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 0));
                _888.e1 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 16));
                _888.e2 = asfloat(geometryTriangles.Load4((b.links.z + i) * 48 + 32));
                tri.a = _888.a;
                tri.e1 = _888.e1;
                tri.e2 = _888.e2;
                float3 p = cross(ld, tri.e2.xyz);
                float det = dot(tri.e1.xyz, p);
                float3 param_20 = tri.e1.xyz;
                float3 param_21 = tri.e2.xyz;
                float3 localN = geometryTriangleNormal(param_20, param_21);
                float3 worldN = ((inst.inverse0.xyz * localN.x) + (inst.inverse1.xyz * localN.y)) + (inst.inverse2.xyz * localN.z);
                float worldNLength = length(worldN);
                float facing = dot(ld, localN);
                bool _947 = any(isnan(localN));
                bool _954;
                if (!_947)
                {
                    _954 = any(isinf(localN));
                }
                else
                {
                    _954 = _947;
                }
                bool _961;
                if (!_954)
                {
                    _961 = any(isnan(worldN));
                }
                else
                {
                    _961 = _954;
                }
                bool _968;
                if (!_961)
                {
                    _968 = any(isinf(worldN));
                }
                else
                {
                    _968 = _961;
                }
                bool _974;
                if (!_968)
                {
                    _974 = isnan(worldNLength);
                }
                else
                {
                    _974 = _968;
                }
                bool _980;
                if (!_974)
                {
                    _980 = isinf(worldNLength);
                }
                else
                {
                    _980 = _974;
                }
                bool _987;
                if (!_980)
                {
                    _987 = !(worldNLength > 0.0f);
                }
                else
                {
                    _987 = _980;
                }
                bool _993;
                if (!_987)
                {
                    _993 = isnan(facing);
                }
                else
                {
                    _993 = _987;
                }
                bool _999;
                if (!_993)
                {
                    _999 = isinf(facing);
                }
                else
                {
                    _999 = _993;
                }
                bool _1005;
                if (!_999)
                {
                    _1005 = isnan(det);
                }
                else
                {
                    _1005 = _999;
                }
                bool _1011;
                if (!_1005)
                {
                    _1011 = isinf(det);
                }
                else
                {
                    _1011 = _1005;
                }
                bool _1014 = _1011 || (det == 0.0f);
                bool _1024;
                if (!_1014)
                {
                    _1024 = abs(facing) <= (9.9999999747524270787835121154785e-07f * worldNLength);
                }
                else
                {
                    _1024 = _1014;
                }
                if (_1024)
                {
                    return -1;
                }
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
                float3 normal = localN;
                float planeDet = dot(ld, normal);
                if (planeDet == 0.0f)
                {
                    return -1;
                }
                float t = dot(tri.a.xyz - lo, normal) / planeDet;
                bool _1087 = isnan(t);
                bool _1093;
                if (!_1087)
                {
                    _1093 = isinf(t);
                }
                else
                {
                    _1093 = _1087;
                }
                bool _1099;
                if (!_1093)
                {
                    _1099 = isnan(bary);
                }
                else
                {
                    _1099 = _1093;
                }
                bool _1105;
                if (!_1099)
                {
                    _1105 = isinf(bary);
                }
                else
                {
                    _1105 = _1099;
                }
                bool _1111;
                if (!_1105)
                {
                    _1111 = isnan(u);
                }
                else
                {
                    _1111 = _1105;
                }
                bool _1117;
                if (!_1111)
                {
                    _1117 = isinf(u);
                }
                else
                {
                    _1117 = _1111;
                }
                if (_1117)
                {
                    return -1;
                }
                bool _1122 = bary < 0.0f;
                bool _1130;
                if (!_1122)
                {
                    _1130 = (u + bary) > 1.0f;
                }
                else
                {
                    _1130 = _1122;
                }
                bool _1134 = _1130 || (t < nearT);
                bool _1141;
                if (!_1134)
                {
                    _1141 = t > bestT;
                }
                else
                {
                    _1141 = _1134;
                }
                if (_1141)
                {
                    continue;
                }
                uint primitive = asuint(tri.a.w);
                bestT = t;
                owner = inst.identity.y;
                bestPrimitive = primitive;
                return 1;
            }
        }
    }
    return int(found);
}

int qualifiedProbeConnection(float3 receiver, float3 probe)
{
    bool _1618 = _1262_connectionPolicy.x != 1u;
    bool _1625;
    if (!_1618)
    {
        _1625 = any(isnan(receiver));
    }
    else
    {
        _1625 = _1618;
    }
    bool _1632;
    if (!_1625)
    {
        _1632 = any(isinf(receiver));
    }
    else
    {
        _1632 = _1625;
    }
    bool _1639;
    if (!_1632)
    {
        _1639 = any(isnan(probe));
    }
    else
    {
        _1639 = _1632;
    }
    bool _1646;
    if (!_1639)
    {
        _1646 = any(isinf(probe));
    }
    else
    {
        _1646 = _1639;
    }
    if (_1646)
    {
        return 0;
    }
    float3 delta = probe - receiver;
    float _distance = length(delta);
    bool _1658 = isnan(_distance);
    bool _1664;
    if (!_1658)
    {
        _1664 = isinf(_distance);
    }
    else
    {
        _1664 = _1658;
    }
    if (_1664)
    {
        return 0;
    }
    if (_distance <= 1.9999999494757503271102905273438e-05f)
    {
        return 0;
    }
    uint _1674;
    geometryTop.GetDimensions(_1674);
    _1674 = (_1674 - 0) / 48;
    bool _1676 = int(_1674) == 0;
    bool _1683;
    if (!_1676)
    {
        uint _1680;
        geometryBottom.GetDimensions(_1680);
        _1680 = (_1680 - 0) / 48;
        _1683 = int(_1680) == 0;
    }
    else
    {
        _1683 = _1676;
    }
    bool _1690;
    if (!_1683)
    {
        uint _1687;
        geometryTriangles.GetDimensions(_1687);
        _1687 = (_1687 - 0) / 48;
        _1690 = int(_1687) == 0;
    }
    else
    {
        _1690 = _1683;
    }
    bool _1697;
    if (!_1690)
    {
        uint _1694;
        geometryInstances.GetDimensions(_1694);
        _1694 = (_1694 - 0) / 64;
        _1697 = int(_1694) == 0;
    }
    else
    {
        _1697 = _1690;
    }
    if (_1697)
    {
        return 0;
    }
    uint3 _1706 = asuint(receiver) & uint3(2139095040u, 2139095040u, 2139095040u);
    bool _1710 = any(bool3(_1706.x >= uint3(1107296256u, 1107296256u, 1107296256u).x, _1706.y >= uint3(1107296256u, 1107296256u, 1107296256u).y, _1706.z >= uint3(1107296256u, 1107296256u, 1107296256u).z));
    bool _1719;
    if (!_1710)
    {
        uint3 _1716 = asuint(probe) & uint3(2139095040u, 2139095040u, 2139095040u);
        _1719 = any(bool3(_1716.x >= uint3(1107296256u, 1107296256u, 1107296256u).x, _1716.y >= uint3(1107296256u, 1107296256u, 1107296256u).y, _1716.z >= uint3(1107296256u, 1107296256u, 1107296256u).z));
    }
    else
    {
        _1719 = _1710;
    }
    if (_1719)
    {
        return 0;
    }
    if (_distance <= 3.9999998989515006542205810546875e-05f)
    {
        return 0;
    }
    float3 direction = delta / _distance.xxx;
    float3 start = receiver - (direction * 1.9999999494757503271102905273438e-05f);
    float3 end = probe + (direction * 1.9999999494757503271102905273438e-05f);
    float farT = _distance + 3.9999998989515006542205810546875e-05f;
    float3 startShift = receiver - start;
    float3 endShift = end - probe;
    float startMove = dot(startShift, direction);
    float endMove = dot(endShift, direction);
    float startSide = length(startShift - (direction * startMove));
    float endSide = length(endShift - (direction * endMove));
    float farExtension = farT - _distance;
    bool _1783 = any(isnan(direction));
    bool _1790;
    if (!_1783)
    {
        _1790 = any(isinf(direction));
    }
    else
    {
        _1790 = _1783;
    }
    bool _1797;
    if (!_1790)
    {
        _1797 = any(isnan(start));
    }
    else
    {
        _1797 = _1790;
    }
    bool _1804;
    if (!_1797)
    {
        _1804 = any(isinf(start));
    }
    else
    {
        _1804 = _1797;
    }
    bool _1811;
    if (!_1804)
    {
        _1811 = any(isnan(end));
    }
    else
    {
        _1811 = _1804;
    }
    bool _1818;
    if (!_1811)
    {
        _1818 = any(isinf(end));
    }
    else
    {
        _1818 = _1811;
    }
    bool _1824;
    if (!_1818)
    {
        _1824 = isnan(farT);
    }
    else
    {
        _1824 = _1818;
    }
    bool _1830;
    if (!_1824)
    {
        _1830 = isinf(farT);
    }
    else
    {
        _1830 = _1824;
    }
    bool _1836;
    if (!_1830)
    {
        _1836 = isnan(farExtension);
    }
    else
    {
        _1836 = _1830;
    }
    bool _1842;
    if (!_1836)
    {
        _1842 = isinf(farExtension);
    }
    else
    {
        _1842 = _1836;
    }
    bool _1850 = (_1842 || (farExtension < 3.6000001273350790143013000488281e-05f)) || (farExtension > 4.4000000343658030033111572265625e-05f);
    bool _1856;
    if (!_1850)
    {
        _1856 = isnan(startSide);
    }
    else
    {
        _1856 = _1850;
    }
    bool _1862;
    if (!_1856)
    {
        _1862 = isinf(startSide);
    }
    else
    {
        _1862 = _1856;
    }
    bool _1868;
    if (!_1862)
    {
        _1868 = isnan(endSide);
    }
    else
    {
        _1868 = _1862;
    }
    bool _1874;
    if (!_1868)
    {
        _1874 = isinf(endSide);
    }
    else
    {
        _1874 = _1868;
    }
    bool _1881 = (_1874 || (startSide > 1.9999999949504854157567024230957e-06f)) || (endSide > 1.9999999949504854157567024230957e-06f);
    bool _1887;
    if (!_1881)
    {
        _1887 = isnan(startMove);
    }
    else
    {
        _1887 = _1881;
    }
    bool _1893;
    if (!_1887)
    {
        _1893 = isinf(startMove);
    }
    else
    {
        _1893 = _1887;
    }
    bool _1899;
    if (!_1893)
    {
        _1899 = isnan(endMove);
    }
    else
    {
        _1899 = _1893;
    }
    bool _1905;
    if (!_1899)
    {
        _1905 = isinf(endMove);
    }
    else
    {
        _1905 = _1899;
    }
    if ((((_1905 || (startMove < 1.8000000636675395071506500244141e-05f)) || (startMove > 2.2000000171829015016555786132812e-05f)) || (endMove < 1.8000000636675395071506500244141e-05f)) || (endMove > 2.2000000171829015016555786132812e-05f))
    {
        return 0;
    }
    float3 param = start;
    float3 param_1 = direction;
    float param_2 = 0.0f;
    float param_3 = farT;
    float param_4;
    uint param_5;
    uint param_6;
    int _1938 = geometryFirstHit(param, param_1, param_2, param_3, param_4, param_5, param_6);
    float t = param_4;
    uint owner = param_5;
    uint primitive = param_6;
    int hit = _1938;
    if (hit < 0)
    {
        return 0;
    }
    if (hit > 0)
    {
        float originalT = t - 1.9999999494757503271102905273438e-05f;
        bool _1956 = !isnan(originalT);
        bool _1962;
        if (_1956)
        {
            _1962 = !isinf(originalT);
        }
        else
        {
            _1962 = _1956;
        }
        bool _1965 = _1962 && (originalT > 1.9999999494757503271102905273438e-05f);
        bool _1972;
        if (_1965)
        {
            _1972 = originalT < (_distance - 1.9999999494757503271102905273438e-05f);
        }
        else
        {
            _1972 = _1965;
        }
        return _1972 ? (-1) : 0;
    }
    return 1;
}

float3 evalProbeRadianceSH9(float3 sh[9], float3 d)
{
    float3 _219 = ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.48860251903533935546875f) * d.y))) + (sh[2] * (0.48860251903533935546875f * d.z))) + (sh[3] * ((-0.48860251903533935546875f) * d.x))) + (sh[4] * ((1.09254848957061767578125f * d.y) * d.x))) + (sh[5] * (((-1.09254848957061767578125f) * d.y) * d.z))) + (sh[6] * (0.3153915703296661376953125f * (((3.0f * d.z) * d.z) - 1.0f)))) + (sh[7] * (((-1.09254848957061767578125f) * d.x) * d.z))) + (sh[8] * (0.5462741851806640625f * ((d.x * d.x) - (d.y * d.y))));
    return _219;
}

void comp_main()
{
    bool measuredVisibility = _1393_cameraPos.w == 7.0f;
    uint2 tid = gl_GlobalInvocationID.xy;
    uint2 tidInGroup = gl_LocalInvocationID.xy;
    uint2 groupSize = uint2(8u, 8u);
    uint _2882_dummy_parameter;
    int2 outSize = int2(spvImageSize(output_tex, _2882_dummy_parameter));
    uint width = uint(outSize.x);
    uint height = uint(outSize.y);
    uint localIdx = (tidInGroup.y * groupSize.x) + tidInGroup.x;
    bool _2904 = tid.x < width;
    bool _2911;
    if (_2904)
    {
        _2911 = tid.y < height;
    }
    else
    {
        _2911 = _2904;
    }
    bool inBounds = _2911;
    float depth = 0.0f;
    float2 uv = 0.0f.xx;
    float3 worldPos = 0.0f.xxx;
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 gp = 0.0f.xxx;
    bool isActive = false;
    float3 physicalConnectionOrigin = 0.0f.xxx;
    bool physicalConnectionValid = false;
    if (inBounds)
    {
        uv = (float2(tid) + 0.5f.xx) / float2(float(width), float(height));
        uint _2936_dummy_parameter;
        int2 fullResSize = int2(spvTextureSize(gbuffer_depth, uint(0), _2936_dummy_parameter));
        int2 fullTexel = int2(float2(fullResSize) * uv);
        depth = gbuffer_depth.Load(int3(fullTexel, 0)).x;
        if (measuredVisibility)
        {
            int2 param = fullTexel;
            float3 param_1;
            bool _2953 = connectionSurfaceOrigin(param, param_1);
            physicalConnectionOrigin = param_1;
            physicalConnectionValid = _2953;
        }
        if (depth < 1.0f)
        {
            float4x4 invVP = float4x4(float4(_1275_viewRows[0]), float4(_1275_viewRows[1]), float4(_1275_viewRows[2]), float4(_1275_viewRows[3]));
            float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
            float4 worldPosH = mul(float4(ndc, depth, 1.0f), invVP);
            worldPos = worldPosH.xyz / worldPosH.w.xxx;
            uint _3016_dummy_parameter;
            int2 fullNormSize = int2(spvTextureSize(gbuffer_normal, uint(0), _3016_dummy_parameter));
            int2 fullNormTexel = int2(float2(fullNormSize) * uv);
            float3 n = gbuffer_normal.Load(int3(fullNormTexel, 0)).xyz;
            n = normalize((n * 2.0f) - 1.0f.xxx);
            bool _3036 = !isnan(n.x);
            bool _3043;
            if (_3036)
            {
                _3043 = !isnan(n.y);
            }
            else
            {
                _3043 = _3036;
            }
            bool _3050;
            if (_3043)
            {
                _3050 = !isnan(n.z);
            }
            else
            {
                _3050 = _3043;
            }
            if (_3050)
            {
                normal = n;
            }
            float3 origin = _2376_probeOriginSpacing.xyz;
            float spacing = _2376_probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing.xxx;
            float3 gridMax = float3(_1393_probeCounts.x - 1.0f, _1393_probeCounts.y - 1.0f, _1393_probeCounts.z - 1.0f);
            bool _3080 = all(bool3(gp.x >= 0.0f.xxx.x, gp.y >= 0.0f.xxx.y, gp.z >= 0.0f.xxx.z));
            bool _3087;
            if (_3080)
            {
                _3087 = all(bool3(gp.x <= gridMax.x, gp.y <= gridMax.y, gp.z <= gridMax.z));
            }
            else
            {
                _3087 = _3080;
            }
            isActive = _3087;
        }
    }
    if (localIdx == 0u)
    {
        tg_cellFound = false;
        tg_cellMin = uint3(0u, 0u, 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    bool _3101 = (localIdx < 8u) && isActive;
    bool _3106;
    if (_3101)
    {
        _3106 = !tg_cellFound;
    }
    else
    {
        _3106 = _3101;
    }
    if (_3106)
    {
        tg_cellMin = uint3(clamp(floor(gp), 0.0f.xxx, float3(_1393_probeCounts.x - 1.0f, _1393_probeCounts.y - 1.0f, _1393_probeCounts.z - 1.0f)));
        tg_cellFound = true;
    }
    GroupMemoryBarrierWithGroupSync();
    uint3 counts = uint3(_1393_probeCounts.xyz);
    if (localIdx < 8u)
    {
        uint _3137;
        if ((localIdx & 1u) != 0u)
        {
            _3137 = min((tg_cellMin.x + 1u), (counts.x - 1u));
        }
        else
        {
            _3137 = tg_cellMin.x;
        }
        uint cx = _3137;
        uint _3156;
        if ((localIdx & 2u) != 0u)
        {
            _3156 = min((tg_cellMin.y + 1u), (counts.y - 1u));
        }
        else
        {
            _3156 = tg_cellMin.y;
        }
        uint cy = _3156;
        uint _3174;
        if ((localIdx & 4u) != 0u)
        {
            _3174 = min((tg_cellMin.z + 1u), (counts.z - 1u));
        }
        else
        {
            _3174 = tg_cellMin.z;
        }
        uint cz = _3174;
        uint pIdx = (cx + (cy * counts.x)) + ((cz * counts.x) * counts.y);
        tg_probe_idx[localIdx] = pIdx;
        tg_probe_depth_base[localIdx] = pIdx * 128u;
    }
    GroupMemoryBarrierWithGroupSync();
    uint probe = localIdx / 9u;
    uint shIdx = localIdx % 9u;
    if (probe < 8u)
    {
        uint pIdx_1 = tg_probe_idx[probe];
        tg_probes[probe].sh[shIdx] = asfloat(_2746.Load4(((pIdx_1 * 9u) + shIdx) * 16 + 0)).xyz;
    }
    if (localIdx < 8u)
    {
        uint idx = 64u + localIdx;
        uint probe_1 = idx / 9u;
        uint shIdx_1 = idx % 9u;
        if ((probe_1 < 8u) && (shIdx_1 < 9u))
        {
            uint pIdx_2 = tg_probe_idx[probe_1];
            tg_probes[probe_1].sh[shIdx_1] = asfloat(_2746.Load4(((pIdx_2 * 9u) + shIdx_1) * 16 + 0)).xyz;
        }
        tg_skyFactor[localIdx] = asfloat(_3284.Load(tg_probe_idx[localIdx] * 4 + 0));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0u; j < 16u; j++)
    {
        uint linearIdx = (localIdx * 16u) + j;
        uint probe_2 = linearIdx / 128u;
        uint offsetInProbe = linearIdx % 128u;
        float val = asfloat(_2054.Load((tg_probe_depth_base[probe_2] + offsetInProbe) * 4 + 0));
        tg_depth_map[(probe_2 * 128u) + offsetInProbe] = val;
    }
    GroupMemoryBarrierWithGroupSync();
    bool _3338;
    if (inBounds)
    {
        _3338 = _1393_debugVis.x > 0.5f;
    }
    else
    {
        _3338 = inBounds;
    }
    if (_3338)
    {
        int visMode = int(_1393_debugVis.x + 0.5f);
        float3 vis = float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
        if (visMode == 4)
        {
            float2 param_2 = uv;
            vis = visProbePositions(param_2);
        }
        else
        {
            if (depth < 1.0f)
            {
                uint3 countsV = uint3(_1393_probeCounts.xyz);
                if (isActive)
                {
                    uint3 b0 = clamp(uint3(floor(gp)), uint3(0u, 0u, 0u), countsV - uint3(1u, 1u, 1u));
                    uint3 b1 = min((b0 + uint3(1u, 1u, 1u)), (countsV - uint3(1u, 1u, 1u)));
                    float3 fracGp = frac(gp);
                    float fx = fracGp.x;
                    float fy = fracGp.y;
                    float fz = fracGp.z;
                    float tw[8];
                    tw[0] = ((1.0f - fx) * (1.0f - fy)) * (1.0f - fz);
                    tw[1] = (fx * (1.0f - fy)) * (1.0f - fz);
                    tw[2] = ((1.0f - fx) * fy) * (1.0f - fz);
                    tw[3] = (fx * fy) * (1.0f - fz);
                    tw[4] = ((1.0f - fx) * (1.0f - fy)) * fz;
                    tw[5] = (fx * (1.0f - fy)) * fz;
                    tw[6] = ((1.0f - fx) * fy) * fz;
                    tw[7] = (fx * fy) * fz;
                    uint3 pc[8];
                    pc[0] = uint3(b0.x, b0.y, b0.z);
                    pc[1] = uint3(b1.x, b0.y, b0.z);
                    pc[2] = uint3(b0.x, b1.y, b0.z);
                    pc[3] = uint3(b1.x, b1.y, b0.z);
                    pc[4] = uint3(b0.x, b0.y, b1.z);
                    pc[5] = uint3(b1.x, b0.y, b1.z);
                    pc[6] = uint3(b0.x, b1.y, b1.z);
                    pc[7] = uint3(b1.x, b1.y, b1.z);
                    if (visMode == 1)
                    {
                        float3 sh0 = 0.0f.xxx;
                        float wsum = 0.0f;
                        for (int p = 0; p < 8; p++)
                        {
                            if (tw[p] < 0.00999999977648258209228515625f)
                            {
                                continue;
                            }
                            uint pIdx_3 = (pc[p].x + (pc[p].y * countsV.x)) + ((pc[p].z * countsV.x) * countsV.y);
                            sh0 += (asfloat(_2746.Load4((pIdx_3 * 9u) * 16 + 0)).xyz * tw[p]);
                            wsum += tw[p];
                        }
                        if (wsum > 0.0f)
                        {
                            sh0 /= wsum.xxx;
                        }
                        vis = sh0 * 0.282094776630401611328125f;
                    }
                    else
                    {
                        if (visMode == 2)
                        {
                            uint3 np = clamp(uint3(floor(gp + 0.5f.xxx)), uint3(0u, 0u, 0u), countsV - uint3(1u, 1u, 1u));
                            uint pIdx_4 = (np.x + (np.y * countsV.x)) + ((np.z * countsV.x) * countsV.y);
                            float3 param_3 = normal;
                            float2 ouv = gatherOctEncode(param_3);
                            int2 texel = int2(int(clamp(uint(ouv.x * 8.0f), 0u, 7u)), int(clamp(uint(ouv.y * 8.0f), 0u, 7u)));
                            float meanDepth = asfloat(_2054.Load(((pIdx_4 * 128u) + uint((texel.y * 8) + texel.x)) * 4 + 0));
                            vis = clamp(meanDepth / 25.0f, 0.0f, 1.0f).xxx;
                        }
                        else
                        {
                            float conf = 0.0f;
                            float wsum_1 = 0.0f;
                            for (int p_1 = 0; p_1 < 8; p_1++)
                            {
                                if (tw[p_1] < 0.00999999977648258209228515625f)
                                {
                                    continue;
                                }
                                uint pIdx_5 = (pc[p_1].x + (pc[p_1].y * countsV.x)) + ((pc[p_1].z * countsV.x) * countsV.y);
                                conf += (asfloat(_3705.Load(((pIdx_5 * 4u) + 3u) * 4 + 0)) * tw[p_1]);
                                wsum_1 += tw[p_1];
                            }
                            vis = clamp(conf / max(wsum_1, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f).xxx;
                        }
                    }
                }
                else
                {
                    bool _3732 = visMode == 1;
                    bool _3744;
                    if (_3732)
                    {
                        _3744 = _3739_fvMeta.x > 1.5f;
                    }
                    else
                    {
                        _3744 = _3732;
                    }
                    bool _3750;
                    if (_3744)
                    {
                        _3750 = _3739_fvOriginSpacing[1].w > 0.0f;
                    }
                    else
                    {
                        _3750 = _3744;
                    }
                    if (_3750)
                    {
                        float3 fgp = (worldPos - _3739_fvOriginSpacing[1].xyz) / _3739_fvOriginSpacing[1].w.xxx;
                        float3 fcounts = _3739_fvCountsOffset[1].xyz;
                        uint foff = uint(_3739_fvCountsOffset[1].w);
                        float3 fgridMax = fcounts - 1.0f.xxx;
                        bool _3777 = all(bool3(fgp.x >= 0.0f.xxx.x, fgp.y >= 0.0f.xxx.y, fgp.z >= 0.0f.xxx.z));
                        bool _3784;
                        if (_3777)
                        {
                            _3784 = all(bool3(fgp.x <= fgridMax.x, fgp.y <= fgridMax.y, fgp.z <= fgridMax.z));
                        }
                        else
                        {
                            _3784 = _3777;
                        }
                        if (_3784)
                        {
                            uint3 fb0 = clamp(uint3(floor(fgp)), uint3(0u, 0u, 0u), uint3(fcounts) - uint3(1u, 1u, 1u));
                            uint3 fb1 = min((fb0 + uint3(1u, 1u, 1u)), (uint3(fcounts) - uint3(1u, 1u, 1u)));
                            float3 ffrac = frac(fgp);
                            float ffx = ffrac.x;
                            float ffy = ffrac.y;
                            float ffz = ffrac.z;
                            float ftw[8];
                            ftw[0] = ((1.0f - ffx) * (1.0f - ffy)) * (1.0f - ffz);
                            ftw[1] = (ffx * (1.0f - ffy)) * (1.0f - ffz);
                            ftw[2] = ((1.0f - ffx) * ffy) * (1.0f - ffz);
                            ftw[3] = (ffx * ffy) * (1.0f - ffz);
                            ftw[4] = ((1.0f - ffx) * (1.0f - ffy)) * ffz;
                            ftw[5] = (ffx * (1.0f - ffy)) * ffz;
                            ftw[6] = ((1.0f - ffx) * ffy) * ffz;
                            ftw[7] = (ffx * ffy) * ffz;
                            uint3 fpc[8];
                            fpc[0] = uint3(fb0.x, fb0.y, fb0.z);
                            fpc[1] = uint3(fb1.x, fb0.y, fb0.z);
                            fpc[2] = uint3(fb0.x, fb1.y, fb0.z);
                            fpc[3] = uint3(fb1.x, fb1.y, fb0.z);
                            fpc[4] = uint3(fb0.x, fb0.y, fb1.z);
                            fpc[5] = uint3(fb1.x, fb0.y, fb1.z);
                            fpc[6] = uint3(fb0.x, fb1.y, fb1.z);
                            fpc[7] = uint3(fb1.x, fb1.y, fb1.z);
                            float3 sh0_1 = 0.0f.xxx;
                            float wsum_2 = 0.0f;
                            for (int p_2 = 0; p_2 < 8; p_2++)
                            {
                                if (ftw[p_2] < 0.00999999977648258209228515625f)
                                {
                                    continue;
                                }
                                uint pIdx_6 = (fpc[p_2].x + (fpc[p_2].y * uint(fcounts.x))) + ((fpc[p_2].z * uint(fcounts.x)) * uint(fcounts.y));
                                sh0_1 += (asfloat(_3987.Load4(((foff + pIdx_6) * 9u) * 16 + 0)).xyz * ftw[p_2]);
                                wsum_2 += ftw[p_2];
                            }
                            if (wsum_2 > 0.0f)
                            {
                                sh0_1 /= wsum_2.xxx;
                            }
                            vis = sh0_1 * 0.282094776630401611328125f;
                        }
                    }
                }
            }
        }
        output_tex[int2(tid)] = float4(vis, 1.0f);
        spec_output_tex[int2(tid)] = 0.0f.xxxx;
    }
    else
    {
        if (_3739_fvMeta.x > 0.5f)
        {
            if (inBounds && (depth < 1.0f))
            {
                int sel = -1;
                float selSpacing = 0.0f;
                int nvolCount = int(_3739_fvMeta.x);
                for (int v = 0; v < nvolCount; v++)
                {
                    if (_3739_fvOriginSpacing[v].w <= 0.0f)
                    {
                        continue;
                    }
                    float3 gpv = (worldPos - _3739_fvOriginSpacing[v].xyz) / _3739_fvOriginSpacing[v].w.xxx;
                    float3 gridMax_1 = _3739_fvCountsOffset[v].xyz - 1.0f.xxx;
                    bool _4086 = all(bool3(gpv.x >= 0.0f.xxx.x, gpv.y >= 0.0f.xxx.y, gpv.z >= 0.0f.xxx.z));
                    bool _4093;
                    if (_4086)
                    {
                        _4093 = all(bool3(gpv.x <= gridMax_1.x, gpv.y <= gridMax_1.y, gpv.z <= gridMax_1.z));
                    }
                    else
                    {
                        _4093 = _4086;
                    }
                    if (_4093)
                    {
                        bool _4097 = sel < 0;
                        bool _4106;
                        if (!_4097)
                        {
                            _4106 = _3739_fvOriginSpacing[v].w < selSpacing;
                        }
                        else
                        {
                            _4106 = _4097;
                        }
                        if (_4106)
                        {
                            sel = v;
                            selSpacing = _3739_fvOriginSpacing[v].w;
                        }
                    }
                }
                bool _4116 = sel >= 0;
                bool _4124;
                if (_4116)
                {
                    _4124 = (!measuredVisibility) || (sel == 0);
                }
                else
                {
                    _4124 = _4116;
                }
                if (_4124)
                {
                    uint volOff = uint(_3739_fvCountsOffset[sel].w);
                    float3 gpn = (worldPos - _3739_fvOriginSpacing[sel].xyz) / _3739_fvOriginSpacing[sel].w.xxx;
                    uint3 ncounts = uint3(_3739_fvCountsOffset[sel].xyz);
                    uint3 b0_1 = clamp(uint3(floor(gpn)), uint3(0u, 0u, 0u), ncounts - uint3(1u, 1u, 1u));
                    uint3 b1_1 = min((b0_1 + uint3(1u, 1u, 1u)), (ncounts - uint3(1u, 1u, 1u)));
                    float3 fracGp_1 = frac(gpn);
                    float fx_1 = fracGp_1.x;
                    float fy_1 = fracGp_1.y;
                    float fz_1 = fracGp_1.z;
                    float tw_1[8];
                    tw_1[0] = ((1.0f - fx_1) * (1.0f - fy_1)) * (1.0f - fz_1);
                    tw_1[1] = (fx_1 * (1.0f - fy_1)) * (1.0f - fz_1);
                    tw_1[2] = ((1.0f - fx_1) * fy_1) * (1.0f - fz_1);
                    tw_1[3] = (fx_1 * fy_1) * (1.0f - fz_1);
                    tw_1[4] = ((1.0f - fx_1) * (1.0f - fy_1)) * fz_1;
                    tw_1[5] = (fx_1 * (1.0f - fy_1)) * fz_1;
                    tw_1[6] = ((1.0f - fx_1) * fy_1) * fz_1;
                    tw_1[7] = (fx_1 * fy_1) * fz_1;
                    uint3 pc_1[8];
                    pc_1[0] = uint3(b0_1.x, b0_1.y, b0_1.z);
                    pc_1[1] = uint3(b1_1.x, b0_1.y, b0_1.z);
                    pc_1[2] = uint3(b0_1.x, b1_1.y, b0_1.z);
                    pc_1[3] = uint3(b1_1.x, b1_1.y, b0_1.z);
                    pc_1[4] = uint3(b0_1.x, b0_1.y, b1_1.z);
                    pc_1[5] = uint3(b1_1.x, b0_1.y, b1_1.z);
                    pc_1[6] = uint3(b0_1.x, b1_1.y, b1_1.z);
                    pc_1[7] = uint3(b1_1.x, b1_1.y, b1_1.z);
                    float3 V = normalize(_1393_cameraPos.xyz - worldPos);
                    float NdotV = clamp(dot(normal, V), 0.0500000007450580596923828125f, 1.0f);
                    float3 R = reflect(-V, normal);
                    float3 result = 0.0f.xxx;
                    float totalW = 0.0f;
                    float3 lowFrequencyGI = 0.0f.xxx;
                    float lowFrequencyW = 0.0f;
                    float confidenceBaseW = 0.0f;
                    float3 reflResult = 0.0f.xxx;
                    float3 bestIrr = 0.0f.xxx;
                    float3 bestRefl = 0.0f.xxx;
                    float bestBaseWeight = -1.0f;
                    float3 biasedPos = worldPos + ((normal * selSpacing) * 0.0500000007450580596923828125f);
                    bool classifyEnable = _3739_fvMeta.y > 0.5f;
                    bool openSkyV2 = _3739_fvMeta.z > 0.5f;
                    float pixOpen = 0.0f;
                    float pixOpenW = 0.0f;
                    if (classifyEnable)
                    {
                        for (int q = 0; q < 8; q++)
                        {
                            if (tw_1[q] < 0.00999999977648258209228515625f)
                            {
                                continue;
                            }
                            uint qIdx = (pc_1[q].x + (pc_1[q].y * ncounts.x)) + ((pc_1[q].z * ncounts.x) * ncounts.y);
                            pixOpen += (asfloat(_4390.Load(((volOff + qIdx) * 2u) * 4 + 0)) * tw_1[q]);
                            pixOpenW += tw_1[q];
                        }
                        if (pixOpenW > 0.0f)
                        {
                            pixOpen /= pixOpenW;
                        }
                    }
                    bool _4426;
                    float3 sh[9];
                    float _4600;
                    float3 _4688;
                    float vw;
                    float _4746;
                    float _4832;
                    for (int p_3 = 0; p_3 < 8; p_3++)
                    {
                        if (measuredVisibility)
                        {
                            _4426 = !(tw_1[p_3] > 0.0f);
                        }
                        else
                        {
                            _4426 = tw_1[p_3] < 0.00999999977648258209228515625f;
                        }
                        if (_4426)
                        {
                            continue;
                        }
                        uint pIdx_7 = (pc_1[p_3].x + (pc_1[p_3].y * ncounts.x)) + ((pc_1[p_3].z * ncounts.x) * ncounts.y);
                        bool _4498;
                        if (measuredVisibility)
                        {
                            uint _4472;
                            _4471.GetDimensions(_4472);
                            _4472 = (_4472 - 0) / 16;
                            bool _4475 = pIdx_7 >= uint(int(_4472));
                            bool _4485;
                            if (!_4475)
                            {
                                uint _4480;
                                _2054.GetDimensions(_4480);
                                _4480 = (_4480 - 0) / 4;
                                _4485 = pIdx_7 >= (uint(int(_4480)) / 128u);
                            }
                            else
                            {
                                _4485 = _4475;
                            }
                            bool _4497;
                            if (!_4485)
                            {
                                uint _4492;
                                _3987.GetDimensions(_4492);
                                _4492 = (_4492 - 0) / 16;
                                _4497 = (volOff + pIdx_7) >= (uint(int(_4492)) / 9u);
                            }
                            else
                            {
                                _4497 = _4485;
                            }
                            _4498 = _4497;
                        }
                        else
                        {
                            _4498 = measuredVisibility;
                        }
                        if (_4498)
                        {
                            continue;
                        }
                        for (int i = 0; i < 9; i++)
                        {
                            sh[i] = asfloat(_3987.Load4((((volOff + pIdx_7) * 9u) + uint(i)) * 16 + 0)).xyz;
                        }
                        if (measuredVisibility)
                        {
                            bool finiteSH = true;
                            for (int i_1 = 0; i_1 < 9; i_1++)
                            {
                                bool _4547;
                                if (finiteSH)
                                {
                                    _4547 = !any(isnan(sh[i_1]));
                                }
                                else
                                {
                                    _4547 = finiteSH;
                                }
                                bool _4556;
                                if (_4547)
                                {
                                    _4556 = !any(isinf(sh[i_1]));
                                }
                                else
                                {
                                    _4556 = _4547;
                                }
                                finiteSH = _4556;
                            }
                            if (!finiteSH)
                            {
                                continue;
                            }
                        }
                        float3 param_4[9] = sh;
                        float3 param_5 = normal;
                        float3 irr = evalProbeIrradianceSH9(param_4, param_5);
                        float w = tw_1[p_3];
                        float lowGate = 1.0f;
                        float atten = 1.0f;
                        if (classifyEnable)
                        {
                            float pOpen = asfloat(_4390.Load(((volOff + pIdx_7) * 2u) * 4 + 0));
                            float skyness = smoothstep(0.1500000059604644775390625f, 0.449999988079071044921875f, pOpen);
                            float enclosed = 1.0f - smoothstep(0.0500000007450580596923828125f, 0.25f, pixOpen);
                            float classify = 1.0f - ((skyness * enclosed) * 0.85000002384185791015625f);
                            if (openSkyV2)
                            {
                                _4600 = lerp(1.0f, classify, asfloat(_4390.Load((((volOff + pIdx_7) * 2u) + 1u) * 4 + 0)));
                            }
                            else
                            {
                                _4600 = classify;
                            }
                            atten = _4600;
                        }
                        w *= atten;
                        float burial = 1.0f;
                        if (sel == 0)
                        {
                            float3 probePos = _3739_fvOriginSpacing[sel].xyz + (float3(pc_1[p_3]) * selSpacing);
                            float originState = 0.0f;
                            bool _4639 = asfloat(_4471.Load(pIdx_7 * 16 + 12)) > 0.25f;
                            bool _4663;
                            if (_4639)
                            {
                                bool _4643 = !measuredVisibility;
                                bool _4662;
                                if (!_4643)
                                {
                                    bool _4652 = !any(isnan(asfloat(_4471.Load4(pIdx_7 * 16 + 0))));
                                    bool _4661;
                                    if (_4652)
                                    {
                                        _4661 = !any(isinf(asfloat(_4471.Load4(pIdx_7 * 16 + 0))));
                                    }
                                    else
                                    {
                                        _4661 = _4652;
                                    }
                                    _4662 = _4661;
                                }
                                else
                                {
                                    _4662 = _4643;
                                }
                                _4663 = _4662;
                            }
                            else
                            {
                                _4663 = _4639;
                            }
                            if (_4663)
                            {
                                probePos = asfloat(_4471.Load4(pIdx_7 * 16 + 0)).xyz;
                                originState = asfloat(_4471.Load(pIdx_7 * 16 + 12));
                            }
                            float3 toProbeVec = probePos - biasedPos;
                            float distToProbe = length(toProbeVec);
                            bool _4687;
                            if (measuredVisibility)
                            {
                                _4687 = !(distToProbe > 9.9999997473787516355514526367188e-06f);
                            }
                            else
                            {
                                _4687 = measuredVisibility;
                            }
                            if (_4687)
                            {
                                _4688 = normal;
                            }
                            else
                            {
                                _4688 = toProbeVec / max(distToProbe, 9.9999997473787516355514526367188e-06f).xxx;
                            }
                            float3 toProbe = _4688;
                            float normalWeight = lerp(0.20000000298023223876953125f, 1.0f, clamp((dot(normal, toProbe) + 0.5f) / 0.699999988079071044921875f, 0.0f, 1.0f));
                            float3 fromProbe = -toProbe;
                            uint param_6 = pIdx_7;
                            float3 param_7 = fromProbe;
                            float2 depthMV = sampleNearProbeDepth(param_6, param_7);
                            float insidePenalty = 1.0f;
                            if (depthMV.x < (selSpacing * 0.20000000298023223876953125f))
                            {
                                insidePenalty = lerp(0.20000000298023223876953125f, 1.0f, smoothstep(0.0f, 0.20000000298023223876953125f, depthMV.x / selSpacing));
                                burial = lerp(0.100000001490116119384765625f, 1.0f, smoothstep(0.0f, 0.20000000298023223876953125f, depthMV.x / selSpacing));
                            }
                            float nwSoft = max(normalWeight, 0.4000000059604644775390625f);
                            if (measuredVisibility)
                            {
                                if (originState > 0.25f)
                                {
                                    float param_8 = distToProbe;
                                    float2 param_9 = depthMV;
                                    _4746 = measuredConnectionVisibility(param_8, param_9);
                                }
                                else
                                {
                                    _4746 = 0.0f;
                                }
                                vw = _4746;
                                if (vw == 0.0f)
                                {
                                    lowGate = 0.0f;
                                }
                            }
                            else
                            {
                                float param_10 = distToProbe;
                                float param_11 = depthMV.x;
                                float param_12 = depthMV.y;
                                float param_13 = selSpacing;
                                vw = visibilityWeight(param_10, param_11, param_12, param_13);
                                vw = max(vw, 0.37999999523162841796875f);
                            }
                            float3 param_14 = probePos;
                            float3 param_15 = biasedPos;
                            float3 param_16 = fromProbe;
                            float param_17 = distToProbe;
                            float2 param_18 = depthMV;
                            if (certifiedFloorBlock(param_14, param_15, param_16, param_17, param_18))
                            {
                                vw = 0.0f;
                                lowGate = 0.0f;
                            }
                            vw *= lerp(0.5f, 1.0f, step(0.75f, originState));
                            if (measuredVisibility)
                            {
                                bool _4801 = !any(isnan(depthMV));
                                bool _4808;
                                if (_4801)
                                {
                                    _4808 = !any(isinf(depthMV));
                                }
                                else
                                {
                                    _4808 = _4801;
                                }
                                bool _4814;
                                if (_4808)
                                {
                                    _4814 = all(bool2(depthMV.x >= 0.0f.xx.x, depthMV.y >= 0.0f.xx.y));
                                }
                                else
                                {
                                    _4814 = _4808;
                                }
                                bool sourceQualified = _4814 && (originState > 0.25f);
                                bool _4821 = sourceQualified && physicalConnectionValid;
                                bool _4830;
                                if (_4821)
                                {
                                    float3 param_19 = physicalConnectionOrigin;
                                    float3 param_20 = probePos;
                                    _4830 = qualifiedProbeConnection(param_19, param_20) == 1;
                                }
                                else
                                {
                                    _4830 = _4821;
                                }
                                bool clearConnection = _4830;
                                if (clearConnection)
                                {
                                    _4832 = lerp(0.5f, 1.0f, step(0.75f, originState));
                                }
                                else
                                {
                                    _4832 = 0.0f;
                                }
                                vw = _4832;
                                lowGate = float(clearConnection);
                            }
                            w *= ((nwSoft * vw) * insidePenalty);
                        }
                        confidenceBaseW += ((tw_1[p_3] * burial) * atten);
                        lowFrequencyGI += ((((irr * tw_1[p_3]) * burial) * atten) * lowGate);
                        lowFrequencyW += (((tw_1[p_3] * burial) * atten) * lowGate);
                        bool _4886 = tw_1[p_3] > bestBaseWeight;
                        bool _4894;
                        if (_4886)
                        {
                            _4894 = (!measuredVisibility) || (w > 0.0f);
                        }
                        else
                        {
                            _4894 = _4886;
                        }
                        if (_4894)
                        {
                            bestBaseWeight = tw_1[p_3];
                            bestIrr = irr;
                        }
                        result += (irr * w);
                        totalW += w;
                        if (sel == 0)
                        {
                            float3 param_21[9] = sh;
                            float3 param_22 = R;
                            float3 reflIrr = evalProbeRadianceSH9(param_21, param_22);
                            reflResult += (reflIrr * w);
                            bool _4928 = tw_1[p_3] >= bestBaseWeight;
                            bool _4936;
                            if (_4928)
                            {
                                _4936 = (!measuredVisibility) || (w > 0.0f);
                            }
                            else
                            {
                                _4936 = _4928;
                            }
                            if (_4936)
                            {
                                bestRefl = reflIrr;
                            }
                        }
                    }
                    if (totalW > 0.0f)
                    {
                        result /= totalW.xxx;
                        reflResult /= totalW.xxx;
                    }
                    else
                    {
                        bool3 _4957 = measuredVisibility.xxx;
                        result = float3(_4957.x ? 0.0f.xxx.x : bestIrr.x, _4957.y ? 0.0f.xxx.y : bestIrr.y, _4957.z ? 0.0f.xxx.z : bestIrr.z);
                        bool3 _4961 = measuredVisibility.xxx;
                        reflResult = float3(_4961.x ? 0.0f.xxx.x : bestRefl.x, _4961.y ? 0.0f.xxx.y : bestRefl.y, _4961.z ? 0.0f.xxx.z : bestRefl.z);
                    }
                    if (lowFrequencyW > 0.0f)
                    {
                        lowFrequencyGI /= lowFrequencyW.xxx;
                        float _4973;
                        if (measuredVisibility)
                        {
                            _4973 = 0.0f;
                        }
                        else
                        {
                            float _4980;
                            if (_3739_fvMeta.w >= 0.0f)
                            {
                                _4980 = clamp(_3739_fvMeta.w, 0.0f, 1.0f);
                            }
                            else
                            {
                                _4980 = 0.300000011920928955078125f;
                            }
                            _4973 = _4980;
                        }
                        float lowFrequencyBlend = _4973;
                        result = lerp(result, lowFrequencyGI, lowFrequencyBlend.xxx);
                    }
                    float visibilityConfidence = clamp(totalW / max(confidenceBaseW, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f);
                    result *= visibilityConfidence;
                    float3 diffOut = max(result * 3.1415927410125732421875f, 0.0f.xxx);
                    bool _5019;
                    if (measuredVisibility)
                    {
                        _5019 = !(totalW > 0.0f);
                    }
                    else
                    {
                        _5019 = measuredVisibility;
                    }
                    output_tex[int2(tid)] = float4(diffOut, _5019 ? 0.0f : 1.0f);
                    if (sel == 0)
                    {
                        uint _5032_dummy_parameter;
                        int2 fullOrmTexel = int2(float2(int2(spvTextureSize(gbuffer_orm, uint(0), _5032_dummy_parameter))) * uv);
                        float roughness = clamp(gbuffer_orm.Load(int3(fullOrmTexel, 0)).y, 0.039999999105930328369140625f, 1.0f);
                        float3 _5046;
                        if (measuredVisibility)
                        {
                            _5046 = max(result, 0.0f.xxx);
                        }
                        else
                        {
                            _5046 = max(result, float3(0.0500000007450580596923828125f, 0.0500000007450580596923828125f, 0.0599999986588954925537109375f));
                        }
                        float3 radBase = _5046;
                        float3 specIrr = lerp(reflResult, radBase, (roughness * roughness).xxx);
                        bool _5074;
                        if (measuredVisibility)
                        {
                            _5074 = !(totalW > 0.0f);
                        }
                        else
                        {
                            _5074 = measuredVisibility;
                        }
                        float4 _5075;
                        if (_5074)
                        {
                            _5075 = 0.0f.xxxx;
                        }
                        else
                        {
                            _5075 = float4(specIrr, NdotV);
                        }
                        spec_output_tex[int2(tid)] = _5075;
                    }
                    else
                    {
                        spec_output_tex[int2(tid)] = 0.0f.xxxx;
                    }
                }
                else
                {
                    bool4 _5096 = measuredVisibility.xxxx;
                    output_tex[int2(tid)] = float4(_5096.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5096.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5096.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5096.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                    spec_output_tex[int2(tid)] = 0.0f.xxxx;
                }
            }
            else
            {
                bool4 _5106 = measuredVisibility.xxxx;
                output_tex[int2(tid)] = float4(_5106.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5106.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5106.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5106.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                spec_output_tex[int2(tid)] = 0.0f.xxxx;
            }
        }
        else
        {
            bool4 _5116 = measuredVisibility.xxxx;
            output_tex[int2(tid)] = float4(_5116.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5116.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5116.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5116.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
            spec_output_tex[int2(tid)] = 0.0f.xxxx;
        }
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
