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

static const float3 _1074[4] = { float3(1.0f, 0.0f, 0.0f), float3(-1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), float3(0.0f, 0.0f, -1.0f) };

ByteAddressBuffer _342 : register(t12, space0);
cbuffer ProbeCounts : register(b6, space0)
{
    float4 _362_probeCounts : packoffset(c0);
    float4 _362_cameraPos : packoffset(c1);
    float4 _362_debugVis : packoffset(c2);
};

cbuffer ViewCB : register(b4, space0)
{
    float4 _599_viewRows[4] : packoffset(c0);
};

cbuffer ProbeOriginSpacing : register(b5, space0)
{
    float4 _681_probeOriginSpacing : packoffset(c0);
};

ByteAddressBuffer _1054 : register(t11, space0);
ByteAddressBuffer _1591 : register(t8, space0);
ByteAddressBuffer _2012 : register(t10, space0);
cbuffer FlatVolumeCB : register(b18, space0)
{
    float4 _2046_fvOriginSpacing[4] : packoffset(c0);
    float4 _2046_fvCountsOffset[4] : packoffset(c4);
    float4 _2046_fvMeta : packoffset(c8);
};

ByteAddressBuffer _2294 : register(t17, space0);
ByteAddressBuffer _2698 : register(t20, space0);
ByteAddressBuffer _2768 : register(t19, space0);
ByteAddressBuffer _3361 : register(t7, space0);
cbuffer StaticProbeCB : register(b9, space0)
{
    GPUStaticProbeData _3366_staticProbe : packoffset(c0);
};

RWTexture2D<float4> output_tex : register(u2, space0);
Texture2D<float4> gbuffer_depth : register(t0, space0);
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

float2 gatherOctEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 uv = d.xy / l1norm.xx;
    if (d.z < 0.0f)
    {
        bool2 _297 = bool2(uv.x >= 0.0f.xx.x, uv.y >= 0.0f.xx.y);
        uv = (1.0f.xx - abs(uv.yx)) * float2(_297.x ? 1.0f.xx.x : (-1.0f).xx.x, _297.y ? 1.0f.xx.y : (-1.0f).xx.y);
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
    return float2(asfloat(_342.Load((base + octIdx) * 4 + 0)), asfloat(_342.Load(((base + 64u) + octIdx) * 4 + 0)));
}

float3 visProbePositions(float2 uv)
{
    float4x4 invVP = float4x4(float4(_599_viewRows[0]), float4(_599_viewRows[1]), float4(_599_viewRows[2]), float4(_599_viewRows[3]));
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 farH = mul(float4(ndc, 0.999000012874603271484375f, 1.0f), invVP);
    float3 ro = _362_cameraPos.xyz;
    float3 rd = (farH.xyz / farH.w.xxx) - ro;
    float rdLen = length(rd);
    if (rdLen < 9.9999997473787516355514526367188e-06f)
    {
        return float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
    }
    rd /= rdLen.xxx;
    float3 vorg = _681_probeOriginSpacing.xyz;
    float sp = max(_681_probeOriginSpacing.w, 0.00999999977648258209228515625f);
    float3 counts = _362_probeCounts.xyz;
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
        bool _850 = any(bool3(cell.x < int3(0, 0, 0).x, cell.y < int3(0, 0, 0).y, cell.z < int3(0, 0, 0).z));
        bool _859;
        if (!_850)
        {
            float3 _855 = float3(cell);
            _859 = any(bool3(_855.x >= counts.x, _855.y >= counts.y, _855.z >= counts.z));
        }
        else
        {
            _859 = _850;
        }
        if (_859)
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
            bool _907 = (th > 0.0f) && (th <= tExit);
            bool _916;
            if (_907)
            {
                _916 = (bestT < 0.0f) || (th < bestT);
            }
            else
            {
                _916 = _907;
            }
            if (_916)
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
        bool _1003 = t > tExit;
        bool _1013;
        if (!_1003)
        {
            _1013 = (bestT > 0.0f) && (t > bestT);
        }
        else
        {
            _1013 = _1003;
        }
        if (_1013)
        {
            break;
        }
    }
    if (bestT < 0.0f)
    {
        return float3(0.100000001490116119384765625f, 0.0199999995529651641845703125f, 0.100000001490116119384765625f);
    }
    uint pIdx = (uint(bestCell.x) + (uint(bestCell.y) * uint(counts.x))) + ((uint(bestCell.z) * uint(counts.x)) * uint(counts.y));
    float3 L0 = asfloat(_1054.Load4((pIdx * 9u) * 16 + 0)).xyz;
    float lum = dot(L0, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    float meanD = 0.0f;
    for (int k = 0; k < 4; k++)
    {
        uint param = pIdx;
        float3 param_1 = _1074[k];
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
    bool _507 = any(isnan(mv));
    bool _514;
    if (!_507)
    {
        _514 = any(isinf(mv));
    }
    else
    {
        _514 = _507;
    }
    bool _521;
    if (!_514)
    {
        _521 = mv.x < 0.0f;
    }
    else
    {
        _521 = _514;
    }
    bool _528;
    if (!_521)
    {
        _528 = mv.y < 0.0f;
    }
    else
    {
        _528 = _521;
    }
    bool _534;
    if (!_528)
    {
        _534 = isnan(_distance);
    }
    else
    {
        _534 = _528;
    }
    bool _540;
    if (!_534)
    {
        _540 = isinf(_distance);
    }
    else
    {
        _540 = _534;
    }
    if (_540)
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
    bool _365 = _362_debugVis.y <= 0.0f;
    bool _372;
    if (!_365)
    {
        _372 = _362_debugVis.z <= 0.0f;
    }
    else
    {
        _372 = _365;
    }
    if (_372)
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
    float plane = _362_debugVis.w;
    bool _420 = probe.y >= (plane - 0.100000001490116119384765625f);
    bool _430;
    if (!_420)
    {
        _430 = receiver.y <= (plane + 0.00999999977648258209228515625f);
    }
    else
    {
        _430 = _420;
    }
    if (_430)
    {
        return false;
    }
    bool _436 = mv.x <= 0.0f;
    bool _443;
    if (!_436)
    {
        _443 = mv.y <= 0.0f;
    }
    else
    {
        _443 = _436;
    }
    bool _459;
    if (!_443)
    {
        _459 = (mv.x + (2.0f * sqrt(mv.y))) >= (_distance - 0.1500000059604644775390625f);
    }
    else
    {
        _459 = _443;
    }
    if (_459)
    {
        return false;
    }
    float t = (plane - probe.y) / (receiver.y - probe.y);
    float3 crossing = lerp(probe, receiver, t.xxx);
    bool _484 = (t > 0.0f) && (t < 1.0f);
    bool _493;
    if (_484)
    {
        _493 = abs(crossing.x) < _362_debugVis.y;
    }
    else
    {
        _493 = _484;
    }
    bool _502;
    if (_493)
    {
        _502 = abs(crossing.z) < _362_debugVis.z;
    }
    else
    {
        _502 = _493;
    }
    return _502;
}

float3 evalProbeRadianceSH9(float3 sh[9], float3 d)
{
    float3 _163 = ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.48860251903533935546875f) * d.y))) + (sh[2] * (0.48860251903533935546875f * d.z))) + (sh[3] * ((-0.48860251903533935546875f) * d.x))) + (sh[4] * ((1.09254848957061767578125f * d.y) * d.x))) + (sh[5] * (((-1.09254848957061767578125f) * d.y) * d.z))) + (sh[6] * (0.3153915703296661376953125f * (((3.0f * d.z) * d.z) - 1.0f)))) + (sh[7] * (((-1.09254848957061767578125f) * d.x) * d.z))) + (sh[8] * (0.5462741851806640625f * ((d.x * d.x) - (d.y * d.y))));
    return _163;
}

void comp_main()
{
    bool measuredVisibility = _362_cameraPos.w == 7.0f;
    uint2 tid = gl_GlobalInvocationID.xy;
    uint2 tidInGroup = gl_LocalInvocationID.xy;
    uint2 groupSize = uint2(8u, 8u);
    uint _1194_dummy_parameter;
    int2 outSize = int2(spvImageSize(output_tex, _1194_dummy_parameter));
    uint width = uint(outSize.x);
    uint height = uint(outSize.y);
    uint localIdx = (tidInGroup.y * groupSize.x) + tidInGroup.x;
    bool _1216 = tid.x < width;
    bool _1223;
    if (_1216)
    {
        _1223 = tid.y < height;
    }
    else
    {
        _1223 = _1216;
    }
    bool inBounds = _1223;
    float depth = 0.0f;
    float2 uv = 0.0f.xx;
    float3 worldPos = 0.0f.xxx;
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 gp = 0.0f.xxx;
    bool isActive = false;
    if (inBounds)
    {
        uv = (float2(tid) + 0.5f.xx) / float2(float(width), float(height));
        uint _1250_dummy_parameter;
        int2 fullResSize = int2(spvTextureSize(gbuffer_depth, uint(0), _1250_dummy_parameter));
        int2 fullTexel = int2(float2(fullResSize) * uv);
        depth = gbuffer_depth.Load(int3(fullTexel, 0)).x;
        if (depth < 1.0f)
        {
            float4x4 invVP = float4x4(float4(_599_viewRows[0]), float4(_599_viewRows[1]), float4(_599_viewRows[2]), float4(_599_viewRows[3]));
            float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
            float4 worldPosH = mul(float4(ndc, depth, 1.0f), invVP);
            worldPos = worldPosH.xyz / worldPosH.w.xxx;
            uint _1322_dummy_parameter;
            int2 fullNormSize = int2(spvTextureSize(gbuffer_normal, uint(0), _1322_dummy_parameter));
            int2 fullNormTexel = int2(float2(fullNormSize) * uv);
            float3 n = gbuffer_normal.Load(int3(fullNormTexel, 0)).xyz;
            n = normalize((n * 2.0f) - 1.0f.xxx);
            bool _1342 = !isnan(n.x);
            bool _1349;
            if (_1342)
            {
                _1349 = !isnan(n.y);
            }
            else
            {
                _1349 = _1342;
            }
            bool _1356;
            if (_1349)
            {
                _1356 = !isnan(n.z);
            }
            else
            {
                _1356 = _1349;
            }
            if (_1356)
            {
                normal = n;
            }
            float3 origin = _681_probeOriginSpacing.xyz;
            float spacing = _681_probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing.xxx;
            float3 gridMax = float3(_362_probeCounts.x - 1.0f, _362_probeCounts.y - 1.0f, _362_probeCounts.z - 1.0f);
            bool _1386 = all(bool3(gp.x >= 0.0f.xxx.x, gp.y >= 0.0f.xxx.y, gp.z >= 0.0f.xxx.z));
            bool _1393;
            if (_1386)
            {
                _1393 = all(bool3(gp.x <= gridMax.x, gp.y <= gridMax.y, gp.z <= gridMax.z));
            }
            else
            {
                _1393 = _1386;
            }
            isActive = _1393;
        }
    }
    if (localIdx == 0u)
    {
        tg_cellFound = false;
        tg_cellMin = uint3(0u, 0u, 0u);
    }
    GroupMemoryBarrierWithGroupSync();
    bool _1407 = (localIdx < 8u) && isActive;
    bool _1412;
    if (_1407)
    {
        _1412 = !tg_cellFound;
    }
    else
    {
        _1412 = _1407;
    }
    if (_1412)
    {
        tg_cellMin = uint3(clamp(floor(gp), 0.0f.xxx, float3(_362_probeCounts.x - 1.0f, _362_probeCounts.y - 1.0f, _362_probeCounts.z - 1.0f)));
        tg_cellFound = true;
    }
    GroupMemoryBarrierWithGroupSync();
    uint3 counts = uint3(_362_probeCounts.xyz);
    if (localIdx < 8u)
    {
        uint _1444;
        if ((localIdx & 1u) != 0u)
        {
            _1444 = min((tg_cellMin.x + 1u), (counts.x - 1u));
        }
        else
        {
            _1444 = tg_cellMin.x;
        }
        uint cx = _1444;
        uint _1463;
        if ((localIdx & 2u) != 0u)
        {
            _1463 = min((tg_cellMin.y + 1u), (counts.y - 1u));
        }
        else
        {
            _1463 = tg_cellMin.y;
        }
        uint cy = _1463;
        uint _1481;
        if ((localIdx & 4u) != 0u)
        {
            _1481 = min((tg_cellMin.z + 1u), (counts.z - 1u));
        }
        else
        {
            _1481 = tg_cellMin.z;
        }
        uint cz = _1481;
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
        tg_probes[probe].sh[shIdx] = asfloat(_1054.Load4(((pIdx_1 * 9u) + shIdx) * 16 + 0)).xyz;
    }
    if (localIdx < 8u)
    {
        uint idx = 64u + localIdx;
        uint probe_1 = idx / 9u;
        uint shIdx_1 = idx % 9u;
        if ((probe_1 < 8u) && (shIdx_1 < 9u))
        {
            uint pIdx_2 = tg_probe_idx[probe_1];
            tg_probes[probe_1].sh[shIdx_1] = asfloat(_1054.Load4(((pIdx_2 * 9u) + shIdx_1) * 16 + 0)).xyz;
        }
        tg_skyFactor[localIdx] = asfloat(_1591.Load(tg_probe_idx[localIdx] * 4 + 0));
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0u; j < 16u; j++)
    {
        uint linearIdx = (localIdx * 16u) + j;
        uint probe_2 = linearIdx / 128u;
        uint offsetInProbe = linearIdx % 128u;
        float val = asfloat(_342.Load((tg_probe_depth_base[probe_2] + offsetInProbe) * 4 + 0));
        tg_depth_map[(probe_2 * 128u) + offsetInProbe] = val;
    }
    GroupMemoryBarrierWithGroupSync();
    bool _1645;
    if (inBounds)
    {
        _1645 = _362_debugVis.x > 0.5f;
    }
    else
    {
        _1645 = inBounds;
    }
    if (_1645)
    {
        int visMode = int(_362_debugVis.x + 0.5f);
        float3 vis = float3(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f);
        if (visMode == 4)
        {
            float2 param = uv;
            vis = visProbePositions(param);
        }
        else
        {
            if (depth < 1.0f)
            {
                uint3 countsV = uint3(_362_probeCounts.xyz);
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
                            sh0 += (asfloat(_1054.Load4((pIdx_3 * 9u) * 16 + 0)).xyz * tw[p]);
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
                            float3 param_1 = normal;
                            float2 ouv = gatherOctEncode(param_1);
                            int2 texel = int2(int(clamp(uint(ouv.x * 8.0f), 0u, 7u)), int(clamp(uint(ouv.y * 8.0f), 0u, 7u)));
                            float meanDepth = asfloat(_342.Load(((pIdx_4 * 128u) + uint((texel.y * 8) + texel.x)) * 4 + 0));
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
                                conf += (asfloat(_2012.Load(((pIdx_5 * 4u) + 3u) * 4 + 0)) * tw[p_1]);
                                wsum_1 += tw[p_1];
                            }
                            vis = clamp(conf / max(wsum_1, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f).xxx;
                        }
                    }
                }
                else
                {
                    bool _2039 = visMode == 1;
                    bool _2051;
                    if (_2039)
                    {
                        _2051 = _2046_fvMeta.x > 1.5f;
                    }
                    else
                    {
                        _2051 = _2039;
                    }
                    bool _2057;
                    if (_2051)
                    {
                        _2057 = _2046_fvOriginSpacing[1].w > 0.0f;
                    }
                    else
                    {
                        _2057 = _2051;
                    }
                    if (_2057)
                    {
                        float3 fgp = (worldPos - _2046_fvOriginSpacing[1].xyz) / _2046_fvOriginSpacing[1].w.xxx;
                        float3 fcounts = _2046_fvCountsOffset[1].xyz;
                        uint foff = uint(_2046_fvCountsOffset[1].w);
                        float3 fgridMax = fcounts - 1.0f.xxx;
                        bool _2084 = all(bool3(fgp.x >= 0.0f.xxx.x, fgp.y >= 0.0f.xxx.y, fgp.z >= 0.0f.xxx.z));
                        bool _2091;
                        if (_2084)
                        {
                            _2091 = all(bool3(fgp.x <= fgridMax.x, fgp.y <= fgridMax.y, fgp.z <= fgridMax.z));
                        }
                        else
                        {
                            _2091 = _2084;
                        }
                        if (_2091)
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
                                sh0_1 += (asfloat(_2294.Load4(((foff + pIdx_6) * 9u) * 16 + 0)).xyz * ftw[p_2]);
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
        if (_2046_fvMeta.x > 0.5f)
        {
            if (inBounds && (depth < 1.0f))
            {
                int sel = -1;
                float selSpacing = 0.0f;
                int nvolCount = int(_2046_fvMeta.x);
                for (int v = 0; v < nvolCount; v++)
                {
                    if (_2046_fvOriginSpacing[v].w <= 0.0f)
                    {
                        continue;
                    }
                    float3 gpv = (worldPos - _2046_fvOriginSpacing[v].xyz) / _2046_fvOriginSpacing[v].w.xxx;
                    float3 gridMax_1 = _2046_fvCountsOffset[v].xyz - 1.0f.xxx;
                    bool _2394 = all(bool3(gpv.x >= 0.0f.xxx.x, gpv.y >= 0.0f.xxx.y, gpv.z >= 0.0f.xxx.z));
                    bool _2401;
                    if (_2394)
                    {
                        _2401 = all(bool3(gpv.x <= gridMax_1.x, gpv.y <= gridMax_1.y, gpv.z <= gridMax_1.z));
                    }
                    else
                    {
                        _2401 = _2394;
                    }
                    if (_2401)
                    {
                        bool _2405 = sel < 0;
                        bool _2414;
                        if (!_2405)
                        {
                            _2414 = _2046_fvOriginSpacing[v].w < selSpacing;
                        }
                        else
                        {
                            _2414 = _2405;
                        }
                        if (_2414)
                        {
                            sel = v;
                            selSpacing = _2046_fvOriginSpacing[v].w;
                        }
                    }
                }
                bool _2424 = sel >= 0;
                bool _2432;
                if (_2424)
                {
                    _2432 = (!measuredVisibility) || (sel == 0);
                }
                else
                {
                    _2432 = _2424;
                }
                if (_2432)
                {
                    uint volOff = uint(_2046_fvCountsOffset[sel].w);
                    float3 gpn = (worldPos - _2046_fvOriginSpacing[sel].xyz) / _2046_fvOriginSpacing[sel].w.xxx;
                    uint3 ncounts = uint3(_2046_fvCountsOffset[sel].xyz);
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
                    float3 V = normalize(_362_cameraPos.xyz - worldPos);
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
                    bool classifyEnable = _2046_fvMeta.y > 0.5f;
                    bool openSkyV2 = _2046_fvMeta.z > 0.5f;
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
                            pixOpen += (asfloat(_2698.Load(((volOff + qIdx) * 2u) * 4 + 0)) * tw_1[q]);
                            pixOpenW += tw_1[q];
                        }
                        if (pixOpenW > 0.0f)
                        {
                            pixOpen /= pixOpenW;
                        }
                    }
                    float3 sh[9];
                    float _2897;
                    float3 _2986;
                    float vw;
                    float _3044;
                    for (int p_3 = 0; p_3 < 8; p_3++)
                    {
                        if (tw_1[p_3] < 0.00999999977648258209228515625f)
                        {
                            continue;
                        }
                        uint pIdx_7 = (pc_1[p_3].x + (pc_1[p_3].y * ncounts.x)) + ((pc_1[p_3].z * ncounts.x) * ncounts.y);
                        bool _2795;
                        if (measuredVisibility)
                        {
                            uint _2769;
                            _2768.GetDimensions(_2769);
                            _2769 = (_2769 - 0) / 16;
                            bool _2772 = pIdx_7 >= uint(int(_2769));
                            bool _2782;
                            if (!_2772)
                            {
                                uint _2777;
                                _342.GetDimensions(_2777);
                                _2777 = (_2777 - 0) / 4;
                                _2782 = pIdx_7 >= (uint(int(_2777)) / 128u);
                            }
                            else
                            {
                                _2782 = _2772;
                            }
                            bool _2794;
                            if (!_2782)
                            {
                                uint _2789;
                                _2294.GetDimensions(_2789);
                                _2789 = (_2789 - 0) / 16;
                                _2794 = (volOff + pIdx_7) >= (uint(int(_2789)) / 9u);
                            }
                            else
                            {
                                _2794 = _2782;
                            }
                            _2795 = _2794;
                        }
                        else
                        {
                            _2795 = measuredVisibility;
                        }
                        if (_2795)
                        {
                            continue;
                        }
                        for (int i = 0; i < 9; i++)
                        {
                            sh[i] = asfloat(_2294.Load4((((volOff + pIdx_7) * 9u) + uint(i)) * 16 + 0)).xyz;
                        }
                        if (measuredVisibility)
                        {
                            bool finiteSH = true;
                            for (int i_1 = 0; i_1 < 9; i_1++)
                            {
                                bool _2844;
                                if (finiteSH)
                                {
                                    _2844 = !any(isnan(sh[i_1]));
                                }
                                else
                                {
                                    _2844 = finiteSH;
                                }
                                bool _2853;
                                if (_2844)
                                {
                                    _2853 = !any(isinf(sh[i_1]));
                                }
                                else
                                {
                                    _2853 = _2844;
                                }
                                finiteSH = _2853;
                            }
                            if (!finiteSH)
                            {
                                continue;
                            }
                        }
                        float3 param_2[9] = sh;
                        float3 param_3 = normal;
                        float3 irr = evalProbeIrradianceSH9(param_2, param_3);
                        float w = tw_1[p_3];
                        float lowGate = 1.0f;
                        float atten = 1.0f;
                        if (classifyEnable)
                        {
                            float pOpen = asfloat(_2698.Load(((volOff + pIdx_7) * 2u) * 4 + 0));
                            float skyness = smoothstep(0.1500000059604644775390625f, 0.449999988079071044921875f, pOpen);
                            float enclosed = 1.0f - smoothstep(0.0500000007450580596923828125f, 0.25f, pixOpen);
                            float classify = 1.0f - ((skyness * enclosed) * 0.85000002384185791015625f);
                            if (openSkyV2)
                            {
                                _2897 = lerp(1.0f, classify, asfloat(_2698.Load((((volOff + pIdx_7) * 2u) + 1u) * 4 + 0)));
                            }
                            else
                            {
                                _2897 = classify;
                            }
                            atten = _2897;
                        }
                        w *= atten;
                        float burial = 1.0f;
                        if (sel == 0)
                        {
                            float3 probePos = _2046_fvOriginSpacing[sel].xyz + (float3(pc_1[p_3]) * selSpacing);
                            float originState = 0.0f;
                            bool _2936 = asfloat(_2768.Load(pIdx_7 * 16 + 12)) > 0.25f;
                            bool _2961;
                            if (_2936)
                            {
                                bool _2940 = !measuredVisibility;
                                bool _2960;
                                if (!_2940)
                                {
                                    bool _2950 = !any(isnan(asfloat(_2768.Load4(pIdx_7 * 16 + 0))));
                                    bool _2959;
                                    if (_2950)
                                    {
                                        _2959 = !any(isinf(asfloat(_2768.Load4(pIdx_7 * 16 + 0))));
                                    }
                                    else
                                    {
                                        _2959 = _2950;
                                    }
                                    _2960 = _2959;
                                }
                                else
                                {
                                    _2960 = _2940;
                                }
                                _2961 = _2960;
                            }
                            else
                            {
                                _2961 = _2936;
                            }
                            if (_2961)
                            {
                                probePos = asfloat(_2768.Load4(pIdx_7 * 16 + 0)).xyz;
                                originState = asfloat(_2768.Load(pIdx_7 * 16 + 12));
                            }
                            float3 toProbeVec = probePos - biasedPos;
                            float distToProbe = length(toProbeVec);
                            bool _2985;
                            if (measuredVisibility)
                            {
                                _2985 = !(distToProbe > 9.9999997473787516355514526367188e-06f);
                            }
                            else
                            {
                                _2985 = measuredVisibility;
                            }
                            if (_2985)
                            {
                                _2986 = normal;
                            }
                            else
                            {
                                _2986 = toProbeVec / max(distToProbe, 9.9999997473787516355514526367188e-06f).xxx;
                            }
                            float3 toProbe = _2986;
                            float normalWeight = lerp(0.20000000298023223876953125f, 1.0f, clamp((dot(normal, toProbe) + 0.5f) / 0.699999988079071044921875f, 0.0f, 1.0f));
                            float3 fromProbe = -toProbe;
                            uint param_4 = pIdx_7;
                            float3 param_5 = fromProbe;
                            float2 depthMV = sampleNearProbeDepth(param_4, param_5);
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
                                    float param_6 = distToProbe;
                                    float2 param_7 = depthMV;
                                    _3044 = measuredConnectionVisibility(param_6, param_7);
                                }
                                else
                                {
                                    _3044 = 0.0f;
                                }
                                vw = _3044;
                                if (vw == 0.0f)
                                {
                                    lowGate = 0.0f;
                                }
                            }
                            else
                            {
                                float param_8 = distToProbe;
                                float param_9 = depthMV.x;
                                float param_10 = depthMV.y;
                                float param_11 = selSpacing;
                                vw = visibilityWeight(param_8, param_9, param_10, param_11);
                                vw = max(vw, 0.37999999523162841796875f);
                            }
                            float3 param_12 = probePos;
                            float3 param_13 = biasedPos;
                            float3 param_14 = fromProbe;
                            float param_15 = distToProbe;
                            float2 param_16 = depthMV;
                            if (certifiedFloorBlock(param_12, param_13, param_14, param_15, param_16))
                            {
                                vw = 0.0f;
                                lowGate = 0.0f;
                            }
                            vw *= lerp(0.5f, 1.0f, step(0.75f, originState));
                            w *= ((nwSoft * vw) * insidePenalty);
                        }
                        confidenceBaseW += ((tw_1[p_3] * burial) * atten);
                        lowFrequencyGI += ((((irr * tw_1[p_3]) * burial) * atten) * lowGate);
                        lowFrequencyW += (((tw_1[p_3] * burial) * atten) * lowGate);
                        if (tw_1[p_3] > bestBaseWeight)
                        {
                            bestBaseWeight = tw_1[p_3];
                            bestIrr = irr;
                        }
                        result += (irr * w);
                        totalW += w;
                        if (sel == 0)
                        {
                            float3 param_17[9] = sh;
                            float3 param_18 = R;
                            float3 reflIrr = evalProbeRadianceSH9(param_17, param_18);
                            reflResult += (reflIrr * w);
                            if (tw_1[p_3] >= bestBaseWeight)
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
                        bool3 _3191 = measuredVisibility.xxx;
                        result = float3(_3191.x ? 0.0f.xxx.x : bestIrr.x, _3191.y ? 0.0f.xxx.y : bestIrr.y, _3191.z ? 0.0f.xxx.z : bestIrr.z);
                        bool3 _3195 = measuredVisibility.xxx;
                        reflResult = float3(_3195.x ? 0.0f.xxx.x : bestRefl.x, _3195.y ? 0.0f.xxx.y : bestRefl.y, _3195.z ? 0.0f.xxx.z : bestRefl.z);
                    }
                    if (lowFrequencyW > 0.0f)
                    {
                        lowFrequencyGI /= lowFrequencyW.xxx;
                        float _3207;
                        if (measuredVisibility)
                        {
                            _3207 = 0.0f;
                        }
                        else
                        {
                            float _3214;
                            if (_2046_fvMeta.w >= 0.0f)
                            {
                                _3214 = clamp(_2046_fvMeta.w, 0.0f, 1.0f);
                            }
                            else
                            {
                                _3214 = 0.300000011920928955078125f;
                            }
                            _3207 = _3214;
                        }
                        float lowFrequencyBlend = _3207;
                        result = lerp(result, lowFrequencyGI, lowFrequencyBlend.xxx);
                    }
                    float visibilityConfidence = clamp(totalW / max(confidenceBaseW, 9.9999997473787516355514526367188e-05f), 0.0f, 1.0f);
                    result *= visibilityConfidence;
                    float3 diffOut = max(result * 3.1415927410125732421875f, 0.0f.xxx);
                    bool _3253;
                    if (measuredVisibility)
                    {
                        _3253 = !(totalW > 0.0f);
                    }
                    else
                    {
                        _3253 = measuredVisibility;
                    }
                    output_tex[int2(tid)] = float4(diffOut, _3253 ? 0.0f : 1.0f);
                    if (sel == 0)
                    {
                        uint _3266_dummy_parameter;
                        int2 fullOrmTexel = int2(float2(int2(spvTextureSize(gbuffer_orm, uint(0), _3266_dummy_parameter))) * uv);
                        float roughness = clamp(gbuffer_orm.Load(int3(fullOrmTexel, 0)).y, 0.039999999105930328369140625f, 1.0f);
                        float3 _3280;
                        if (measuredVisibility)
                        {
                            _3280 = max(result, 0.0f.xxx);
                        }
                        else
                        {
                            _3280 = max(result, float3(0.0500000007450580596923828125f, 0.0500000007450580596923828125f, 0.0599999986588954925537109375f));
                        }
                        float3 radBase = _3280;
                        float3 specIrr = lerp(reflResult, radBase, (roughness * roughness).xxx);
                        bool _3308;
                        if (measuredVisibility)
                        {
                            _3308 = !(totalW > 0.0f);
                        }
                        else
                        {
                            _3308 = measuredVisibility;
                        }
                        float4 _3309;
                        if (_3308)
                        {
                            _3309 = 0.0f.xxxx;
                        }
                        else
                        {
                            _3309 = float4(specIrr, NdotV);
                        }
                        spec_output_tex[int2(tid)] = _3309;
                    }
                    else
                    {
                        spec_output_tex[int2(tid)] = 0.0f.xxxx;
                    }
                }
                else
                {
                    bool4 _3330 = measuredVisibility.xxxx;
                    output_tex[int2(tid)] = float4(_3330.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _3330.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _3330.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _3330.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                    spec_output_tex[int2(tid)] = 0.0f.xxxx;
                }
            }
            else
            {
                bool4 _3340 = measuredVisibility.xxxx;
                output_tex[int2(tid)] = float4(_3340.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _3340.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _3340.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _3340.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
                spec_output_tex[int2(tid)] = 0.0f.xxxx;
            }
        }
        else
        {
            bool4 _3350 = measuredVisibility.xxxx;
            output_tex[int2(tid)] = float4(_3350.x ? 0.0f.xxxx.x : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).x, _3350.y ? 0.0f.xxxx.y : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).y, _3350.z ? 0.0f.xxxx.z : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).z, _3350.w ? 0.0f.xxxx.w : float4(0.02999999932944774627685546875f, 0.02999999932944774627685546875f, 0.0350000001490116119384765625f, 1.0f).w);
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
