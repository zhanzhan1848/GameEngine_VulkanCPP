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

static const float3 _2774[4] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f) };

ByteAddressBuffer geometryTop : register(t23, space0);
ByteAddressBuffer geometryInstances : register(t26, space0);
ByteAddressBuffer geometryBottom : register(t24, space0);
ByteAddressBuffer geometryTriangles : register(t25, space0);
cbuffer ConnectionPolicy : register(b27, space0)
{
    uint4 _1270_connectionPolicy : packoffset(c0);
    float4 _1270_connectionJitter : packoffset(c1);
};

cbuffer ViewCB : register(b4, space0)
{
    float4 _1283_viewRows[4] : packoffset(c0);
};

cbuffer ProbeCounts : register(b6, space0)
{
    float4 _1401_probeCounts : packoffset(c0);
    float4 _1401_cameraPos : packoffset(c1);
    float4 _1401_debugVis : packoffset(c2);
};

ByteAddressBuffer _2062 : register(t12, space0);
cbuffer ProbeOriginSpacing : register(b5, space0)
{
    float4 _2384_probeOriginSpacing : packoffset(c0);
};

ByteAddressBuffer _2754 : register(t11, space0);
ByteAddressBuffer _3292 : register(t8, space0);
ByteAddressBuffer _3713 : register(t10, space0);
cbuffer FlatVolumeCB : register(b18, space0)
{
    float4 _3747_fvOriginSpacing[4] : packoffset(c0);
    float4 _3747_fvCountsOffset[4] : packoffset(c4);
    float4 _3747_fvMeta : packoffset(c8);
};

ByteAddressBuffer _3995 : register(t17, space0);
ByteAddressBuffer _4398 : register(t20, space0);
ByteAddressBuffer _4479 : register(t19, space0);
ByteAddressBuffer _5135 : register(t7, space0);
cbuffer StaticProbeCB : register(b9, space0)
{
    GPUStaticProbeData _5140_staticProbe : packoffset(c0);
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
    uint _1208_dummy_parameter;
    int2 size = int2(spvTextureSize(gbuffer_depth, uint(0), _1208_dummy_parameter));
    bool _1213 = any(bool2(pixel.x < int2(0, 0).x, pixel.y < int2(0, 0).y));
    bool _1221;
    if (!_1213)
    {
        _1221 = any(bool2(pixel.x >= size.x, pixel.y >= size.y));
    }
    else
    {
        _1221 = _1213;
    }
    if (_1221)
    {
        return false;
    }
    float z = gbuffer_depth.Load(int3(pixel, 0)).x;
    bool _1231 = isnan(z);
    bool _1237;
    if (!_1231)
    {
        _1237 = isinf(z);
    }
    else
    {
        _1237 = _1231;
    }
    if ((_1237 || (z < 0.0f)) || (z >= 0.999998986721038818359375f))
    {
        return false;
    }
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(size);
    float2 ndc = float2((2.0f * uv.x) - 1.0f, 1.0f - (2.0f * uv.y)) - _1270_connectionJitter.xy;
    float4x4 invVP = float4x4(float4(_1283_viewRows[0]), float4(_1283_viewRows[1]), float4(_1283_viewRows[2]), float4(_1283_viewRows[3]));
    float4 h = mul(float4(ndc, z, 1.0f), invVP);
    bool _1324 = any(isnan(h));
    bool _1331;
    if (!_1324)
    {
        _1331 = any(isinf(h));
    }
    else
    {
        _1331 = _1324;
    }
    bool _1340;
    if (!_1331)
    {
        _1340 = abs(h.w) < 9.9999999392252902907785028219223e-09f;
    }
    else
    {
        _1340 = _1331;
    }
    if (_1340)
    {
        return false;
    }
    p = h.xyz / h.w.xxx;
    bool _1353 = !any(isnan(p));
    bool _1360;
    if (_1353)
    {
        _1360 = !any(isinf(p));
    }
    else
    {
        _1360 = _1353;
    }
    return _1360;
}

bool connectionSurfaceOrigin(int2 pixel, inout float3 origin)
{
    bool _1366 = _1270_connectionPolicy.x != 1u;
    bool _1375;
    if (!_1366)
    {
        _1375 = any(isnan(_1270_connectionJitter.xy));
    }
    else
    {
        _1375 = _1366;
    }
    bool _1384;
    if (!_1375)
    {
        _1384 = any(isinf(_1270_connectionJitter.xy));
    }
    else
    {
        _1384 = _1375;
    }
    if (_1384)
    {
        return false;
    }
    int2 param = pixel;
    float3 param_1;
    bool _1392 = connectionSurfacePoint(param, param_1);
    float3 p = param_1;
    if (!_1392)
    {
        return false;
    }
    float3 v = _1401_cameraPos.xyz - p;
    bool _1409 = any(isnan(v));
    bool _1416;
    if (!_1409)
    {
        _1416 = any(isinf(v));
    }
    else
    {
        _1416 = _1409;
    }
    bool _1425;
    if (!_1416)
    {
        _1425 = dot(v, v) < 1.0000000133514319600180897396058e-10f;
    }
    else
    {
        _1425 = _1416;
    }
    if (_1425)
    {
        return false;
    }
    int2 param_2 = pixel - int2(1, 0);
    float3 param_3;
    bool _1436 = connectionSurfacePoint(param_2, param_3);
    float3 a = param_3;
    bool ax = _1436;
    int2 param_4 = pixel + int2(1, 0);
    float3 param_5;
    bool _1444 = connectionSurfacePoint(param_4, param_5);
    float3 b = param_5;
    bool bx = _1444;
    int2 param_6 = pixel - int2(0, 1);
    float3 param_7;
    bool _1453 = connectionSurfacePoint(param_6, param_7);
    float3 c = param_7;
    bool cy = _1453;
    int2 param_8 = pixel + int2(0, 1);
    float3 param_9;
    bool _1461 = connectionSurfacePoint(param_8, param_9);
    float3 d = param_9;
    bool dy = _1461;
    bool _1467 = (!ax) && (!bx);
    bool _1476;
    if (!_1467)
    {
        _1476 = (!cy) && (!dy);
    }
    else
    {
        _1476 = _1467;
    }
    if (_1476)
    {
        return false;
    }
    bool _1497;
    if (ax)
    {
        bool _1485 = !bx;
        bool _1496;
        if (!_1485)
        {
            _1496 = distance(a, p) < distance(b, p);
        }
        else
        {
            _1496 = _1485;
        }
        _1497 = _1496;
    }
    else
    {
        _1497 = ax;
    }
    float3 _1498;
    if (_1497)
    {
        _1498 = p - a;
    }
    else
    {
        _1498 = b - p;
    }
    float3 dx = _1498;
    bool _1526;
    if (cy)
    {
        bool _1514 = !dy;
        bool _1525;
        if (!_1514)
        {
            _1525 = distance(c, p) < distance(d, p);
        }
        else
        {
            _1525 = _1514;
        }
        _1526 = _1525;
    }
    else
    {
        _1526 = cy;
    }
    float3 _1527;
    if (_1526)
    {
        _1527 = p - c;
    }
    else
    {
        _1527 = d - p;
    }
    float3 ddy = _1527;
    float limit = max(0.0199999995529651641845703125f, length(v) * 0.0199999995529651641845703125f);
    bool _1547 = length(dx) > limit;
    bool _1555;
    if (!_1547)
    {
        _1555 = length(ddy) > limit;
    }
    else
    {
        _1555 = _1547;
    }
    if (_1555)
    {
        return false;
    }
    float3 crossN = cross(dx, ddy);
    float scale = max(abs(crossN.x), max(abs(crossN.y), abs(crossN.z)));
    bool _1577 = any(isnan(crossN));
    bool _1584;
    if (!_1577)
    {
        _1584 = any(isinf(crossN));
    }
    else
    {
        _1584 = _1577;
    }
    if (_1584 || (scale < 1.000000016862383526387164645044e-16f))
    {
        return false;
    }
    float3 ng = normalize(crossN / scale.xxx);
    if (dot(ng, v) < 0.0f)
    {
        ng = -ng;
    }
    origin = p + (ng * 0.00200000009499490261077880859375f);
    bool _1614 = !any(isnan(origin));
    bool _1621;
    if (_1614)
    {
        _1621 = !any(isinf(origin));
    }
    else
    {
        _1621 = _1614;
    }
    return _1621;
}

float2 gatherOctEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 uv = d.xy / l1norm.xx;
    if (d.z < 0.0f)
    {
        bool2 _2018 = bool2(uv.x >= 0.0f.xx.x, uv.y >= 0.0f.xx.y);
        uv = (1.0f.xx - abs(uv.yx)) * float2(_2018.x ? 1.0f.xx.x : (-1.0f).xx.x, _2018.y ? 1.0f.xx.y : (-1.0f).xx.y);
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
    return float2(asfloat(_2062.Load((base + octIdx) * 4 + 0)), asfloat(_2062.Load(((base + 64u) + octIdx) * 4 + 0)));
}

float3 visProbePositions(float2 uv)
{
    float4x4 invVP = float4x4(float4(_1283_viewRows[0]), float4(_1283_viewRows[1]), float4(_1283_viewRows[2]), float4(_1283_viewRows[3]));
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 farH = mul(float4(ndc, 0.999000012874603271484375f, 1.0f), invVP);
    float3 ro = _1401_cameraPos.xyz;
    float3 rd = (farH.xyz / farH.w.xxx) - ro;
    float rdLen = length(rd);
    if (rdLen < 9.9999997473787516355514526367188e-06f)
    {
        return float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
    }
    rd /= rdLen.xxx;
    float3 vorg = _2384_probeOriginSpacing.xyz;
    float sp = max(_2384_probeOriginSpacing.w, 0.00999999977648258209228515625f);
    float3 counts = _1401_probeCounts.xyz;
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
        bool _2551 = any(bool3(cell.x < int3(0, 0, 0).x, cell.y < int3(0, 0, 0).y, cell.z < int3(0, 0, 0).z));
        bool _2560;
        if (!_2551)
        {
            float3 _2556 = float3(cell);
            _2560 = any(bool3(_2556.x >= counts.x, _2556.y >= counts.y, _2556.z >= counts.z));
        }
        else
        {
            _2560 = _2551;
        }
        if (_2560)
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
            bool _2608 = (th > 0.0f) && (th <= tExit);
            bool _2617;
            if (_2608)
            {
                _2617 = (bestT < 0.0f) || (th < bestT);
            }
            else
            {
                _2617 = _2608;
            }
            if (_2617)
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
        bool _2704 = t > tExit;
        bool _2714;
        if (!_2704)
        {
            _2714 = (bestT > 0.0f) && (t > bestT);
        }
        else
        {
            _2714 = _2704;
        }
        if (_2714)
        {
            break;
        }
    }
    if (bestT < 0.0f)
    {
        return float3(0.100000001490116119384765625f, 0.0199999995529651641845703125f, 0.100000001490116119384765625f);
    }
    uint pIdx = (uint(bestCell.x) + (uint(bestCell.y) * uint(counts.x))) + ((uint(bestCell.z) * uint(counts.x)) * uint(counts.y));
    float3 L0 = asfloat(_2754.Load4((pIdx * 9u) * 16 + 0)).xyz;
    float lum = dot(L0, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    float meanD = 0.0f;
    for (int k = 0; k < 4; k++)
    {
        uint param = pIdx;
        float3 param_1 = _2774[k];
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
    bool _2219 = any(isnan(mv));
    bool _2226;
    if (!_2219)
    {
        _2226 = any(isinf(mv));
    }
    else
    {
        _2226 = _2219;
    }
    bool _2233;
    if (!_2226)
    {
        _2233 = mv.x < 0.0f;
    }
    else
    {
        _2233 = _2226;
    }
    bool _2240;
    if (!_2233)
    {
        _2240 = mv.y < 0.0f;
    }
    else
    {
        _2240 = _2233;
    }
    bool _2246;
    if (!_2240)
    {
        _2246 = isnan(_distance);
    }
    else
    {
        _2246 = _2240;
    }
    bool _2252;
    if (!_2246)
    {
        _2252 = isinf(_distance);
    }
    else
    {
        _2252 = _2246;
    }
    if (_2252)
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
    bool _2080 = _1401_debugVis.y <= 0.0f;
    bool _2087;
    if (!_2080)
    {
        _2087 = _1401_debugVis.z <= 0.0f;
    }
    else
    {
        _2087 = _2080;
    }
    if (_2087)
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
    float plane = _1401_debugVis.w;
    bool _2133 = probe.y >= (plane - 0.100000001490116119384765625f);
    bool _2143;
    if (!_2133)
    {
        _2143 = receiver.y <= (plane + 0.00999999977648258209228515625f);
    }
    else
    {
        _2143 = _2133;
    }
    if (_2143)
    {
        return false;
    }
    bool _2149 = mv.x <= 0.0f;
    bool _2156;
    if (!_2149)
    {
        _2156 = mv.y <= 0.0f;
    }
    else
    {
        _2156 = _2149;
    }
    bool _2171;
    if (!_2156)
    {
        _2171 = (mv.x + (2.0f * sqrt(mv.y))) >= (_distance - 0.1500000059604644775390625f);
    }
    else
    {
        _2171 = _2156;
    }
    if (_2171)
    {
        return false;
    }
    float t = (plane - probe.y) / (receiver.y - probe.y);
    float3 crossing = lerp(probe, receiver, t.xxx);
    bool _2196 = (t > 0.0f) && (t < 1.0f);
    bool _2205;
    if (_2196)
    {
        _2205 = abs(crossing.x) < _1401_debugVis.y;
    }
    else
    {
        _2205 = _2196;
    }
    bool _2214;
    if (_2205)
    {
        _2214 = abs(crossing.z) < _1401_debugVis.z;
    }
    else
    {
        _2214 = _2205;
    }
    return _2214;
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
                float3 param_22 = tri.e1.xyz;
                float3 param_23 = tri.e2.xyz;
                float3 normal = geometryTriangleNormal(param_22, param_23);
                float planeDet = dot(ld, normal);
                if (planeDet == 0.0f)
                {
                    return -1;
                }
                float t = dot(tri.a.xyz - lo, normal) / planeDet;
                bool _1095 = isnan(t);
                bool _1101;
                if (!_1095)
                {
                    _1101 = isinf(t);
                }
                else
                {
                    _1101 = _1095;
                }
                bool _1107;
                if (!_1101)
                {
                    _1107 = isnan(bary);
                }
                else
                {
                    _1107 = _1101;
                }
                bool _1113;
                if (!_1107)
                {
                    _1113 = isinf(bary);
                }
                else
                {
                    _1113 = _1107;
                }
                bool _1119;
                if (!_1113)
                {
                    _1119 = isnan(u);
                }
                else
                {
                    _1119 = _1113;
                }
                bool _1125;
                if (!_1119)
                {
                    _1125 = isinf(u);
                }
                else
                {
                    _1125 = _1119;
                }
                if (_1125)
                {
                    return -1;
                }
                bool _1130 = bary < 0.0f;
                bool _1138;
                if (!_1130)
                {
                    _1138 = (u + bary) > 1.0f;
                }
                else
                {
                    _1138 = _1130;
                }
                bool _1142 = _1138 || (t < nearT);
                bool _1149;
                if (!_1142)
                {
                    _1149 = t > bestT;
                }
                else
                {
                    _1149 = _1142;
                }
                if (_1149)
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
    bool _1626 = _1270_connectionPolicy.x != 1u;
    bool _1633;
    if (!_1626)
    {
        _1633 = any(isnan(receiver));
    }
    else
    {
        _1633 = _1626;
    }
    bool _1640;
    if (!_1633)
    {
        _1640 = any(isinf(receiver));
    }
    else
    {
        _1640 = _1633;
    }
    bool _1647;
    if (!_1640)
    {
        _1647 = any(isnan(probe));
    }
    else
    {
        _1647 = _1640;
    }
    bool _1654;
    if (!_1647)
    {
        _1654 = any(isinf(probe));
    }
    else
    {
        _1654 = _1647;
    }
    if (_1654)
    {
        return 0;
    }
    float3 delta = probe - receiver;
    float _distance = length(delta);
    bool _1666 = isnan(_distance);
    bool _1672;
    if (!_1666)
    {
        _1672 = isinf(_distance);
    }
    else
    {
        _1672 = _1666;
    }
    if (_1672)
    {
        return 0;
    }
    if (_distance <= 1.9999999494757503271102905273438e-05f)
    {
        return 0;
    }
    uint _1682;
    geometryTop.GetDimensions(_1682);
    _1682 = (_1682 - 0) / 48;
    bool _1684 = int(_1682) == 0;
    bool _1691;
    if (!_1684)
    {
        uint _1688;
        geometryBottom.GetDimensions(_1688);
        _1688 = (_1688 - 0) / 48;
        _1691 = int(_1688) == 0;
    }
    else
    {
        _1691 = _1684;
    }
    bool _1698;
    if (!_1691)
    {
        uint _1695;
        geometryTriangles.GetDimensions(_1695);
        _1695 = (_1695 - 0) / 48;
        _1698 = int(_1695) == 0;
    }
    else
    {
        _1698 = _1691;
    }
    bool _1705;
    if (!_1698)
    {
        uint _1702;
        geometryInstances.GetDimensions(_1702);
        _1702 = (_1702 - 0) / 64;
        _1705 = int(_1702) == 0;
    }
    else
    {
        _1705 = _1698;
    }
    if (_1705)
    {
        return 0;
    }
    uint3 _1714 = asuint(receiver) & uint3(2139095040u, 2139095040u, 2139095040u);
    bool _1718 = any(bool3(_1714.x >= uint3(1107296256u, 1107296256u, 1107296256u).x, _1714.y >= uint3(1107296256u, 1107296256u, 1107296256u).y, _1714.z >= uint3(1107296256u, 1107296256u, 1107296256u).z));
    bool _1727;
    if (!_1718)
    {
        uint3 _1724 = asuint(probe) & uint3(2139095040u, 2139095040u, 2139095040u);
        _1727 = any(bool3(_1724.x >= uint3(1107296256u, 1107296256u, 1107296256u).x, _1724.y >= uint3(1107296256u, 1107296256u, 1107296256u).y, _1724.z >= uint3(1107296256u, 1107296256u, 1107296256u).z));
    }
    else
    {
        _1727 = _1718;
    }
    if (_1727)
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
    bool _1791 = any(isnan(direction));
    bool _1798;
    if (!_1791)
    {
        _1798 = any(isinf(direction));
    }
    else
    {
        _1798 = _1791;
    }
    bool _1805;
    if (!_1798)
    {
        _1805 = any(isnan(start));
    }
    else
    {
        _1805 = _1798;
    }
    bool _1812;
    if (!_1805)
    {
        _1812 = any(isinf(start));
    }
    else
    {
        _1812 = _1805;
    }
    bool _1819;
    if (!_1812)
    {
        _1819 = any(isnan(end));
    }
    else
    {
        _1819 = _1812;
    }
    bool _1826;
    if (!_1819)
    {
        _1826 = any(isinf(end));
    }
    else
    {
        _1826 = _1819;
    }
    bool _1832;
    if (!_1826)
    {
        _1832 = isnan(farT);
    }
    else
    {
        _1832 = _1826;
    }
    bool _1838;
    if (!_1832)
    {
        _1838 = isinf(farT);
    }
    else
    {
        _1838 = _1832;
    }
    bool _1844;
    if (!_1838)
    {
        _1844 = isnan(farExtension);
    }
    else
    {
        _1844 = _1838;
    }
    bool _1850;
    if (!_1844)
    {
        _1850 = isinf(farExtension);
    }
    else
    {
        _1850 = _1844;
    }
    bool _1858 = (_1850 || (farExtension < 3.6000001273350790143013000488281e-05f)) || (farExtension > 4.4000000343658030033111572265625e-05f);
    bool _1864;
    if (!_1858)
    {
        _1864 = isnan(startSide);
    }
    else
    {
        _1864 = _1858;
    }
    bool _1870;
    if (!_1864)
    {
        _1870 = isinf(startSide);
    }
    else
    {
        _1870 = _1864;
    }
    bool _1876;
    if (!_1870)
    {
        _1876 = isnan(endSide);
    }
    else
    {
        _1876 = _1870;
    }
    bool _1882;
    if (!_1876)
    {
        _1882 = isinf(endSide);
    }
    else
    {
        _1882 = _1876;
    }
    bool _1889 = (_1882 || (startSide > 1.9999999949504854157567024230957e-06f)) || (endSide > 1.9999999949504854157567024230957e-06f);
    bool _1895;
    if (!_1889)
    {
        _1895 = isnan(startMove);
    }
    else
    {
        _1895 = _1889;
    }
    bool _1901;
    if (!_1895)
    {
        _1901 = isinf(startMove);
    }
    else
    {
        _1901 = _1895;
    }
    bool _1907;
    if (!_1901)
    {
        _1907 = isnan(endMove);
    }
    else
    {
        _1907 = _1901;
    }
    bool _1913;
    if (!_1907)
    {
        _1913 = isinf(endMove);
    }
    else
    {
        _1913 = _1907;
    }
    if ((((_1913 || (startMove < 1.8000000636675395071506500244141e-05f)) || (startMove > 2.2000000171829015016555786132812e-05f)) || (endMove < 1.8000000636675395071506500244141e-05f)) || (endMove > 2.2000000171829015016555786132812e-05f))
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
    int _1946 = geometryFirstHit(param, param_1, param_2, param_3, param_4, param_5, param_6);
    float t = param_4;
    uint owner = param_5;
    uint primitive = param_6;
    int hit = _1946;
    if (hit < 0)
    {
        return 0;
    }
    if (hit > 0)
    {
        float originalT = t - 1.9999999494757503271102905273438e-05f;
        bool _1964 = !isnan(originalT);
        bool _1970;
        if (_1964)
        {
            _1970 = !isinf(originalT);
        }
        else
        {
            _1970 = _1964;
        }
        bool _1973 = _1970 && (originalT > 1.9999999494757503271102905273438e-05f);
        bool _1980;
        if (_1973)
        {
            _1980 = originalT < (_distance - 1.9999999494757503271102905273438e-05f);
        }
        else
        {
            _1980 = _1973;
        }
        return _1980 ? (-1) : 0;
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
    bool measuredVisibility = _1401_cameraPos.w == 7.0f;
    uint2 tid = gl_GlobalInvocationID.xy;
    uint2 tidInGroup = gl_LocalInvocationID.xy;
    uint2 groupSize = uint2(8u, 8u);
    uint _2890_dummy_parameter;
    int2 outSize = int2(spvImageSize(output_tex, _2890_dummy_parameter));
    uint width = uint(outSize.x);
    uint height = uint(outSize.y);
    uint localIdx = (tidInGroup.y * groupSize.x) + tidInGroup.x;
    bool _2912 = tid.x < width;
    bool _2919;
    if (_2912)
    {
        _2919 = tid.y < height;
    }
    else
    {
        _2919 = _2912;
    }
    bool inBounds = _2919;
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
        uint _2944_dummy_parameter;
        int2 fullResSize = int2(spvTextureSize(gbuffer_depth, uint(0), _2944_dummy_parameter));
        int2 fullTexel = int2(float2(fullResSize) * uv);
        depth = gbuffer_depth.Load(int3(fullTexel, 0)).x;
        if (measuredVisibility)
        {
            int2 param = fullTexel;
            float3 param_1;
            bool _2961 = connectionSurfaceOrigin(param, param_1);
            physicalConnectionOrigin = param_1;
            physicalConnectionValid = _2961;
        }
        if (depth < 1.0f)
        {
            float4x4 invVP = float4x4(float4(_1283_viewRows[0]), float4(_1283_viewRows[1]), float4(_1283_viewRows[2]), float4(_1283_viewRows[3]));
            float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
            float4 worldPosH = mul(float4(ndc, depth, 1.0f), invVP);
            worldPos = worldPosH.xyz / worldPosH.w.xxx;
            uint _3024_dummy_parameter;
            int2 fullNormSize = int2(spvTextureSize(gbuffer_normal, uint(0), _3024_dummy_parameter));
            int2 fullNormTexel = int2(float2(fullNormSize) * uv);
            float3 n = gbuffer_normal.Load(int3(fullNormTexel, 0)).xyz;
            n = normalize((n * 2.0f) - 1.0f.xxx);
            bool _3044 = !isnan(n.x);
            bool _3051;
            if (_3044)
            {
                _3051 = !isnan(n.y);
            }
            else
            {
                _3051 = _3044;
            }
            bool _3058;
            if (_3051)
            {
                _3058 = !isnan(n.z);
            }
            else
            {
                _3058 = _3051;
            }
            if (_3058)
            {
                normal = n;
            }
            float3 origin = _2384_probeOriginSpacing.xyz;
            float spacing = _2384_probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing.xxx;
            float3 gridMax = float3(_1401_probeCounts.x - 1.0f, _1401_probeCounts.y - 1.0f, _1401_probeCounts.z - 1.0f);
            bool _3088 = all(bool3(gp.x >= 0.0f.xxx.x, gp.y >= 0.0f.xxx.y, gp.z >= 0.0f.xxx.z));
            bool _3095;
            if (_3088)
            {
                _3095 = all(bool3(gp.x <= gridMax.x, gp.y <= gridMax.y, gp.z <= gridMax.z));
            }
            else
            {
                _3095 = _3088;
            }
            isActive = _3095;
        }
    }
    if (localIdx == 0u)
    {
        tg_cellFound = false;
        tg_cellMin = uint3(0u, 0u, 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    bool _3109 = (localIdx < 8u) && isActive;
    bool _3114;
    if (_3109)
    {
        _3114 = !tg_cellFound;
    }
    else
    {
        _3114 = _3109;
    }
    if (_3114)
    {
        tg_cellMin = uint3(clamp(floor(gp), 0.0f.xxx, float3(_1401_probeCounts.x - 1.0f, _1401_probeCounts.y - 1.0f, _1401_probeCounts.z - 1.0f)));
        tg_cellFound = true;
    }
    GroupMemoryBarrierWithGroupSync();
    uint3 counts = uint3(_1401_probeCounts.xyz);
    if (localIdx < 8u)
    {
        uint _3145;
        if ((localIdx & 1u) != 0u)
        {
            _3145 = min((tg_cellMin.x + 1u), (counts.x - 1u));
        }
        else
        {
            _3145 = tg_cellMin.x;
        }
        uint cx = _3145;
        uint _3164;
        if ((localIdx & 2u) != 0u)
        {
            _3164 = min((tg_cellMin.y + 1u), (counts.y - 1u));
        }
        else
        {
            _3164 = tg_cellMin.y;
        }
        uint cy = _3164;
        uint _3182;
        if ((localIdx & 4u) != 0u)
        {
            _3182 = min((tg_cellMin.z + 1u), (counts.z - 1u));
        }
        else
        {
            _3182 = tg_cellMin.z;
        }
        uint cz = _3182;
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
        tg_probes[probe].sh[shIdx] = asfloat(_2754.Load4(((pIdx_1 * 9u) + shIdx) * 16 + 0)).xyz;
    }
    if (localIdx < 8u)
    {
        uint idx = 64u + localIdx;
        uint probe_1 = idx / 9u;
        uint shIdx_1 = idx % 9u;
        if ((probe_1 < 8u) && (shIdx_1 < 9u))
        {
            uint pIdx_2 = tg_probe_idx[probe_1];
            tg_probes[probe_1].sh[shIdx_1] = asfloat(_2754.Load4(((pIdx_2 * 9u) + shIdx_1) * 16 + 0)).xyz;
        }
        tg_skyFactor[localIdx] = asfloat(_3292.Load(tg_probe_idx[localIdx] * 4 + 0));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0u; j < 16u; j++)
    {
        uint linearIdx = (localIdx * 16u) + j;
        uint probe_2 = linearIdx / 128u;
        uint offsetInProbe = linearIdx % 128u;
        float val = asfloat(_2062.Load((tg_probe_depth_base[probe_2] + offsetInProbe) * 4 + 0));
        tg_depth_map[(probe_2 * 128u) + offsetInProbe] = val;
    }
    GroupMemoryBarrierWithGroupSync();
    bool _3346;
    if (inBounds)
    {
        _3346 = _1401_debugVis.x > 0.5f;
    }
    else
    {
        _3346 = inBounds;
    }
    if (_3346)
    {
        int visMode = int(_1401_debugVis.x + 0.5f);
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
                uint3 countsV = uint3(_1401_probeCounts.xyz);
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
                            sh0 += (asfloat(_2754.Load4((pIdx_3 * 9u) * 16 + 0)).xyz * tw[p]);
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
                            float meanDepth = asfloat(_2062.Load(((pIdx_4 * 128u) + uint((texel.y * 8) + texel.x)) * 4 + 0));
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
                                conf += (asfloat(_3713.Load(((pIdx_5 * 4u) + 3u) * 4 + 0)) * tw[p_1]);
                                wsum_1 += tw[p_1];
                            }
                            vis = clamp(conf / max(wsum_1, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f).xxx;
                        }
                    }
                }
                else
                {
                    bool _3740 = visMode == 1;
                    bool _3752;
                    if (_3740)
                    {
                        _3752 = _3747_fvMeta.x > 1.5f;
                    }
                    else
                    {
                        _3752 = _3740;
                    }
                    bool _3758;
                    if (_3752)
                    {
                        _3758 = _3747_fvOriginSpacing[1].w > 0.0f;
                    }
                    else
                    {
                        _3758 = _3752;
                    }
                    if (_3758)
                    {
                        float3 fgp = (worldPos - _3747_fvOriginSpacing[1].xyz) / _3747_fvOriginSpacing[1].w.xxx;
                        float3 fcounts = _3747_fvCountsOffset[1].xyz;
                        uint foff = uint(_3747_fvCountsOffset[1].w);
                        float3 fgridMax = fcounts - 1.0f.xxx;
                        bool _3785 = all(bool3(fgp.x >= 0.0f.xxx.x, fgp.y >= 0.0f.xxx.y, fgp.z >= 0.0f.xxx.z));
                        bool _3792;
                        if (_3785)
                        {
                            _3792 = all(bool3(fgp.x <= fgridMax.x, fgp.y <= fgridMax.y, fgp.z <= fgridMax.z));
                        }
                        else
                        {
                            _3792 = _3785;
                        }
                        if (_3792)
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
                                sh0_1 += (asfloat(_3995.Load4(((foff + pIdx_6) * 9u) * 16 + 0)).xyz * ftw[p_2]);
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
        if (_3747_fvMeta.x > 0.5f)
        {
            if (inBounds && (depth < 1.0f))
            {
                int sel = -1;
                float selSpacing = 0.0f;
                int nvolCount = int(_3747_fvMeta.x);
                for (int v = 0; v < nvolCount; v++)
                {
                    if (_3747_fvOriginSpacing[v].w <= 0.0f)
                    {
                        continue;
                    }
                    float3 gpv = (worldPos - _3747_fvOriginSpacing[v].xyz) / _3747_fvOriginSpacing[v].w.xxx;
                    float3 gridMax_1 = _3747_fvCountsOffset[v].xyz - 1.0f.xxx;
                    bool _4094 = all(bool3(gpv.x >= 0.0f.xxx.x, gpv.y >= 0.0f.xxx.y, gpv.z >= 0.0f.xxx.z));
                    bool _4101;
                    if (_4094)
                    {
                        _4101 = all(bool3(gpv.x <= gridMax_1.x, gpv.y <= gridMax_1.y, gpv.z <= gridMax_1.z));
                    }
                    else
                    {
                        _4101 = _4094;
                    }
                    if (_4101)
                    {
                        bool _4105 = sel < 0;
                        bool _4114;
                        if (!_4105)
                        {
                            _4114 = _3747_fvOriginSpacing[v].w < selSpacing;
                        }
                        else
                        {
                            _4114 = _4105;
                        }
                        if (_4114)
                        {
                            sel = v;
                            selSpacing = _3747_fvOriginSpacing[v].w;
                        }
                    }
                }
                bool _4124 = sel >= 0;
                bool _4132;
                if (_4124)
                {
                    _4132 = (!measuredVisibility) || (sel == 0);
                }
                else
                {
                    _4132 = _4124;
                }
                if (_4132)
                {
                    uint volOff = uint(_3747_fvCountsOffset[sel].w);
                    float3 gpn = (worldPos - _3747_fvOriginSpacing[sel].xyz) / _3747_fvOriginSpacing[sel].w.xxx;
                    uint3 ncounts = uint3(_3747_fvCountsOffset[sel].xyz);
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
                    float3 V = normalize(_1401_cameraPos.xyz - worldPos);
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
                    bool classifyEnable = _3747_fvMeta.y > 0.5f;
                    bool openSkyV2 = _3747_fvMeta.z > 0.5f;
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
                            pixOpen += (asfloat(_4398.Load(((volOff + qIdx) * 2u) * 4 + 0)) * tw_1[q]);
                            pixOpenW += tw_1[q];
                        }
                        if (pixOpenW > 0.0f)
                        {
                            pixOpen /= pixOpenW;
                        }
                    }
                    bool _4434;
                    float3 sh[9];
                    float _4608;
                    float3 _4696;
                    float vw;
                    float _4754;
                    float _4840;
                    for (int p_3 = 0; p_3 < 8; p_3++)
                    {
                        if (measuredVisibility)
                        {
                            _4434 = !(tw_1[p_3] > 0.0f);
                        }
                        else
                        {
                            _4434 = tw_1[p_3] < 0.00999999977648258209228515625f;
                        }
                        if (_4434)
                        {
                            continue;
                        }
                        uint pIdx_7 = (pc_1[p_3].x + (pc_1[p_3].y * ncounts.x)) + ((pc_1[p_3].z * ncounts.x) * ncounts.y);
                        bool _4506;
                        if (measuredVisibility)
                        {
                            uint _4480;
                            _4479.GetDimensions(_4480);
                            _4480 = (_4480 - 0) / 16;
                            bool _4483 = pIdx_7 >= uint(int(_4480));
                            bool _4493;
                            if (!_4483)
                            {
                                uint _4488;
                                _2062.GetDimensions(_4488);
                                _4488 = (_4488 - 0) / 4;
                                _4493 = pIdx_7 >= (uint(int(_4488)) / 128u);
                            }
                            else
                            {
                                _4493 = _4483;
                            }
                            bool _4505;
                            if (!_4493)
                            {
                                uint _4500;
                                _3995.GetDimensions(_4500);
                                _4500 = (_4500 - 0) / 16;
                                _4505 = (volOff + pIdx_7) >= (uint(int(_4500)) / 9u);
                            }
                            else
                            {
                                _4505 = _4493;
                            }
                            _4506 = _4505;
                        }
                        else
                        {
                            _4506 = measuredVisibility;
                        }
                        if (_4506)
                        {
                            continue;
                        }
                        for (int i = 0; i < 9; i++)
                        {
                            sh[i] = asfloat(_3995.Load4((((volOff + pIdx_7) * 9u) + uint(i)) * 16 + 0)).xyz;
                        }
                        if (measuredVisibility)
                        {
                            bool finiteSH = true;
                            for (int i_1 = 0; i_1 < 9; i_1++)
                            {
                                bool _4555;
                                if (finiteSH)
                                {
                                    _4555 = !any(isnan(sh[i_1]));
                                }
                                else
                                {
                                    _4555 = finiteSH;
                                }
                                bool _4564;
                                if (_4555)
                                {
                                    _4564 = !any(isinf(sh[i_1]));
                                }
                                else
                                {
                                    _4564 = _4555;
                                }
                                finiteSH = _4564;
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
                            float pOpen = asfloat(_4398.Load(((volOff + pIdx_7) * 2u) * 4 + 0));
                            float skyness = smoothstep(0.1500000059604644775390625f, 0.449999988079071044921875f, pOpen);
                            float enclosed = 1.0f - smoothstep(0.0500000007450580596923828125f, 0.25f, pixOpen);
                            float classify = 1.0f - ((skyness * enclosed) * 0.85000002384185791015625f);
                            if (openSkyV2)
                            {
                                _4608 = lerp(1.0f, classify, asfloat(_4398.Load((((volOff + pIdx_7) * 2u) + 1u) * 4 + 0)));
                            }
                            else
                            {
                                _4608 = classify;
                            }
                            atten = _4608;
                        }
                        w *= atten;
                        float burial = 1.0f;
                        if (sel == 0)
                        {
                            float3 probePos = _3747_fvOriginSpacing[sel].xyz + (float3(pc_1[p_3]) * selSpacing);
                            float originState = 0.0f;
                            bool _4647 = asfloat(_4479.Load(pIdx_7 * 16 + 12)) > 0.25f;
                            bool _4671;
                            if (_4647)
                            {
                                bool _4651 = !measuredVisibility;
                                bool _4670;
                                if (!_4651)
                                {
                                    bool _4660 = !any(isnan(asfloat(_4479.Load4(pIdx_7 * 16 + 0))));
                                    bool _4669;
                                    if (_4660)
                                    {
                                        _4669 = !any(isinf(asfloat(_4479.Load4(pIdx_7 * 16 + 0))));
                                    }
                                    else
                                    {
                                        _4669 = _4660;
                                    }
                                    _4670 = _4669;
                                }
                                else
                                {
                                    _4670 = _4651;
                                }
                                _4671 = _4670;
                            }
                            else
                            {
                                _4671 = _4647;
                            }
                            if (_4671)
                            {
                                probePos = asfloat(_4479.Load4(pIdx_7 * 16 + 0)).xyz;
                                originState = asfloat(_4479.Load(pIdx_7 * 16 + 12));
                            }
                            float3 toProbeVec = probePos - biasedPos;
                            float distToProbe = length(toProbeVec);
                            bool _4695;
                            if (measuredVisibility)
                            {
                                _4695 = !(distToProbe > 9.9999997473787516355514526367188e-06f);
                            }
                            else
                            {
                                _4695 = measuredVisibility;
                            }
                            if (_4695)
                            {
                                _4696 = normal;
                            }
                            else
                            {
                                _4696 = toProbeVec / max(distToProbe, 9.9999997473787516355514526367188e-06f).xxx;
                            }
                            float3 toProbe = _4696;
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
                                    _4754 = measuredConnectionVisibility(param_8, param_9);
                                }
                                else
                                {
                                    _4754 = 0.0f;
                                }
                                vw = _4754;
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
                                bool _4809 = !any(isnan(depthMV));
                                bool _4816;
                                if (_4809)
                                {
                                    _4816 = !any(isinf(depthMV));
                                }
                                else
                                {
                                    _4816 = _4809;
                                }
                                bool _4822;
                                if (_4816)
                                {
                                    _4822 = all(bool2(depthMV.x >= 0.0f.xx.x, depthMV.y >= 0.0f.xx.y));
                                }
                                else
                                {
                                    _4822 = _4816;
                                }
                                bool sourceQualified = _4822 && (originState > 0.25f);
                                bool _4829 = sourceQualified && physicalConnectionValid;
                                bool _4838;
                                if (_4829)
                                {
                                    float3 param_19 = physicalConnectionOrigin;
                                    float3 param_20 = probePos;
                                    _4838 = qualifiedProbeConnection(param_19, param_20) == 1;
                                }
                                else
                                {
                                    _4838 = _4829;
                                }
                                bool clearConnection = _4838;
                                if (clearConnection)
                                {
                                    _4840 = lerp(0.5f, 1.0f, step(0.75f, originState));
                                }
                                else
                                {
                                    _4840 = 0.0f;
                                }
                                vw = _4840;
                                lowGate = float(clearConnection);
                            }
                            w *= ((nwSoft * vw) * insidePenalty);
                        }
                        confidenceBaseW += ((tw_1[p_3] * burial) * atten);
                        lowFrequencyGI += ((((irr * tw_1[p_3]) * burial) * atten) * lowGate);
                        lowFrequencyW += (((tw_1[p_3] * burial) * atten) * lowGate);
                        bool _4894 = tw_1[p_3] > bestBaseWeight;
                        bool _4902;
                        if (_4894)
                        {
                            _4902 = (!measuredVisibility) || (w > 0.0f);
                        }
                        else
                        {
                            _4902 = _4894;
                        }
                        if (_4902)
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
                            bool _4936 = tw_1[p_3] >= bestBaseWeight;
                            bool _4944;
                            if (_4936)
                            {
                                _4944 = (!measuredVisibility) || (w > 0.0f);
                            }
                            else
                            {
                                _4944 = _4936;
                            }
                            if (_4944)
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
                        bool3 _4965 = measuredVisibility.xxx;
                        result = float3(_4965.x ? 0.0f.xxx.x : bestIrr.x, _4965.y ? 0.0f.xxx.y : bestIrr.y, _4965.z ? 0.0f.xxx.z : bestIrr.z);
                        bool3 _4969 = measuredVisibility.xxx;
                        reflResult = float3(_4969.x ? 0.0f.xxx.x : bestRefl.x, _4969.y ? 0.0f.xxx.y : bestRefl.y, _4969.z ? 0.0f.xxx.z : bestRefl.z);
                    }
                    if (lowFrequencyW > 0.0f)
                    {
                        lowFrequencyGI /= lowFrequencyW.xxx;
                        float _4981;
                        if (measuredVisibility)
                        {
                            _4981 = 0.0f;
                        }
                        else
                        {
                            float _4988;
                            if (_3747_fvMeta.w >= 0.0f)
                            {
                                _4988 = clamp(_3747_fvMeta.w, 0.0f, 1.0f);
                            }
                            else
                            {
                                _4988 = 0.300000011920928955078125f;
                            }
                            _4981 = _4988;
                        }
                        float lowFrequencyBlend = _4981;
                        result = lerp(result, lowFrequencyGI, lowFrequencyBlend.xxx);
                    }
                    float visibilityConfidence = clamp(totalW / max(confidenceBaseW, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f);
                    result *= visibilityConfidence;
                    float3 diffOut = max(result * 3.1415927410125732421875f, 0.0f.xxx);
                    bool _5027;
                    if (measuredVisibility)
                    {
                        _5027 = !(totalW > 0.0f);
                    }
                    else
                    {
                        _5027 = measuredVisibility;
                    }
                    output_tex[int2(tid)] = float4(diffOut, _5027 ? 0.0f : 1.0f);
                    if (sel == 0)
                    {
                        uint _5040_dummy_parameter;
                        int2 fullOrmTexel = int2(float2(int2(spvTextureSize(gbuffer_orm, uint(0), _5040_dummy_parameter))) * uv);
                        float roughness = clamp(gbuffer_orm.Load(int3(fullOrmTexel, 0)).y, 0.039999999105930328369140625f, 1.0f);
                        float3 _5054;
                        if (measuredVisibility)
                        {
                            _5054 = max(result, 0.0f.xxx);
                        }
                        else
                        {
                            _5054 = max(result, float3(0.0500000007450580596923828125f, 0.0500000007450580596923828125f, 0.0599999986588954925537109375f));
                        }
                        float3 radBase = _5054;
                        float3 specIrr = lerp(reflResult, radBase, (roughness * roughness).xxx);
                        bool _5082;
                        if (measuredVisibility)
                        {
                            _5082 = !(totalW > 0.0f);
                        }
                        else
                        {
                            _5082 = measuredVisibility;
                        }
                        float4 _5083;
                        if (_5082)
                        {
                            _5083 = 0.0f.xxxx;
                        }
                        else
                        {
                            _5083 = float4(specIrr, NdotV);
                        }
                        spec_output_tex[int2(tid)] = _5083;
                    }
                    else
                    {
                        spec_output_tex[int2(tid)] = 0.0f.xxxx;
                    }
                }
                else
                {
                    bool4 _5104 = measuredVisibility.xxxx;
                    output_tex[int2(tid)] = float4(_5104.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5104.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5104.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5104.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                    spec_output_tex[int2(tid)] = 0.0f.xxxx;
                }
            }
            else
            {
                bool4 _5114 = measuredVisibility.xxxx;
                output_tex[int2(tid)] = float4(_5114.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5114.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5114.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5114.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                spec_output_tex[int2(tid)] = 0.0f.xxxx;
            }
        }
        else
        {
            bool4 _5124 = measuredVisibility.xxxx;
            output_tex[int2(tid)] = float4(_5124.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _5124.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _5124.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _5124.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
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
