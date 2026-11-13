static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer AvgConstantsBlock : register(b2, space0)
{
    uint constants_totalProbes : packoffset(c0);
    uint constants_raysPerProbe : packoffset(c0.y);
    float constants_frameSeed : packoffset(c0.z);
    float constants_isMode : packoffset(c0.w);
    uint constants_tier3Mode : packoffset(c1);
    uint constants_tierRaysMode : packoffset(c1.y);
    uint constants_sh2CheckMode : packoffset(c1.z);
    uint constants_pad0 : packoffset(c1.w);
    float constants_skyConfFloor : packoffset(c2);
    float constants_pad1 : packoffset(c2.y);
};

ByteAddressBuffer slotToCompact : register(t5, space0);
RWByteAddressBuffer probeSHOut : register(u1, space0);
ByteAddressBuffer probeNormals : register(t3, space0);
ByteAddressBuffer rayRadiance : register(t0, space0);
ByteAddressBuffer importanceSH : register(t4, space0);
RWByteAddressBuffer sh2Check : register(u6, space0);
RWByteAddressBuffer probeOpenSky : register(u7, space0);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

groupshared float3 sharedSH[4][64];
groupshared float sharedWeight[64];
groupshared float sharedHit[64];
groupshared float sharedActive[64];
groupshared float sharedLum[64];
groupshared float sharedLumSq[64];
groupshared float3 sharedRayDir[64];
groupshared float3 sharedRayRad[64];
groupshared float3 sharedErrSum[64];

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
    uint _167 = spgiJitterHash(param);
    uint h1 = _167;
    uint param_1 = h1 ^ 1757159915u;
    uint _173 = spgiJitterHash(param_1);
    uint h2 = _173;
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
    bool3 _242 = (abs(probeNormal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_242.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _242.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _242.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
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
    uint _370;
    if (frameSeed < 0.0f)
    {
        _370 = 0u;
    }
    else
    {
        _370 = uint(frameSeed);
    }
    uint sampleSeed = _370;
    uint param_4 = ((probeIdx * 2654435769u) ^ sampleSeed) ^ (rayIdx * 2246822507u);
    uint _387 = spgiJitterHash(param_4);
    uint sampleHash = _387;
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
    bool3 _97 = (abs(normal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_97.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _97.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _97.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(((tangent * dir.x) + (bitangent * dir.y)) + (normal * dir.z));
}

float4 shBasis(float3 d)
{
    return float4(0.2820949852466583251953125f, (-0.48860299587249755859375f) * d.y, 0.48860299587249755859375f * d.z, (-0.48860299587249755859375f) * d.x);
}

void comp_main()
{
    uint tid = gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    if (group_id >= constants_totalProbes)
    {
        return;
    }
    uint radianceBase = group_id * constants_raysPerProbe;
    float tierRays = float(constants_raysPerProbe);
    if (constants_tier3Mode > 0u)
    {
        uint compact = slotToCompact.Load(group_id * 4 + 0);
        if (compact == 4294967295u)
        {
            if (tid == 0u)
            {
                uint base = group_id * 4u;
                probeSHOut.Store4((base + 0u) * 16 + 0, asuint(0.0f.xxxx));
                probeSHOut.Store4((base + 1u) * 16 + 0, asuint(0.0f.xxxx));
                probeSHOut.Store4((base + 2u) * 16 + 0, asuint(0.0f.xxxx));
                probeSHOut.Store4((base + 3u) * 16 + 0, asuint(0.0f.xxxx));
            }
            return;
        }
        radianceBase = compact * constants_raysPerProbe;
        if (constants_tierRaysMode > 0u)
        {
            tierRays = asfloat(probeNormals.Load(group_id * 16 + 12));
        }
    }
    uint rayIdx = radianceBase + tid;
    float4 rayData = 0.0f.xxxx;
    bool _569 = float(tid) < tierRays;
    bool _576;
    if (_569)
    {
        _576 = tid < constants_raysPerProbe;
    }
    else
    {
        _576 = _569;
    }
    if (_576)
    {
        rayData = asfloat(rayRadiance.Load4(rayIdx * 16 + 0));
    }
    if (rayData.w != 0.0f)
    {
        float rayLum = dot(rayData.xyz, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        if (rayLum > 64.0f)
        {
            float4 _602 = rayData;
            float3 _604 = _602.xyz * (64.0f / rayLum);
            rayData.x = _604.x;
            rayData.y = _604.y;
            rayData.z = _604.z;
        }
        float4 normData = asfloat(probeNormals.Load4(group_id * 16 + 0));
        float3 probeNormal = normalize(normData.xyz);
        float invNq = 1.0f;
        float3 rayDir;
        if (constants_isMode > 0.5f)
        {
            uint hb = group_id * 4u;
            float4 h0 = asfloat(importanceSH.Load4((hb + 0u) * 16 + 0));
            float4 h1 = asfloat(importanceSH.Load4((hb + 1u) * 16 + 0));
            float4 h2 = asfloat(importanceSH.Load4((hb + 2u) * 16 + 0));
            float4 h3 = asfloat(importanceSH.Load4((hb + 3u) * 16 + 0));
            uint param = tid;
            uint param_1 = constants_raysPerProbe;
            uint param_2 = group_id;
            float param_3 = constants_frameSeed;
            float3 param_4 = probeNormal;
            float3 param_5 = h0.xyz;
            float3 param_6 = h1.xyz;
            float3 param_7 = h2.xyz;
            float3 param_8 = h3.xyz;
            float param_9;
            float3 _680 = spgiImportanceDirection(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
            invNq = param_9;
            rayDir = _680;
            float3 param_10 = rayDir;
            float3 param_11 = probeNormal;
            rayDir = orientAroundNormalTBN(param_10, param_11);
        }
        else
        {
            uint param_12 = tid;
            uint param_13 = constants_raysPerProbe;
            float3 localDir = fibonacciDirection(param_12, param_13);
            uint param_14 = group_id;
            float param_15 = constants_frameSeed;
            localDir = mul(localDir, spgiJitterRotation(param_14, param_15));
            float3 param_16 = localDir;
            float3 param_17 = probeNormal;
            rayDir = orientAroundNormalTBN(param_16, param_17);
        }
        float3 param_18 = rayDir;
        float4 Y = shBasis(param_18);
        float rayWeight = abs(rayData.w);
        float projWeight = invNq;
        float3 weightedRadiance = rayData.xyz * projWeight;
        sharedSH[0][tid] = weightedRadiance * Y.x;
        sharedSH[1][tid] = weightedRadiance * Y.y;
        sharedSH[2][tid] = weightedRadiance * Y.z;
        sharedSH[3][tid] = weightedRadiance * Y.w;
        sharedWeight[tid] = rayWeight;
        sharedHit[tid] = float(rayData.w > 0.0f);
        sharedActive[tid] = 1.0f;
        float lum = dot(rayData.xyz, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
        sharedLum[tid] = lum;
        sharedLumSq[tid] = lum * lum;
        sharedRayDir[tid] = rayDir;
        sharedRayRad[tid] = rayData.xyz;
    }
    else
    {
        sharedSH[0][tid] = 0.0f.xxx;
        sharedSH[1][tid] = 0.0f.xxx;
        sharedSH[2][tid] = 0.0f.xxx;
        sharedSH[3][tid] = 0.0f.xxx;
        sharedWeight[tid] = 0.0f;
        sharedLum[tid] = 0.0f;
        sharedLumSq[tid] = 0.0f;
        sharedHit[tid] = 0.0f;
        sharedActive[tid] = 0.0f;
        sharedRayDir[tid] = 0.0f.xxx;
        sharedRayRad[tid] = 0.0f.xxx;
        sharedErrSum[tid] = 0.0f.xxx;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32u; stride > 0u; stride = stride >> 1u)
    {
        if (tid < stride)
        {
            sharedSH[0][tid] += sharedSH[0][tid + stride];
            sharedSH[1][tid] += sharedSH[1][tid + stride];
            sharedSH[2][tid] += sharedSH[2][tid + stride];
            sharedSH[3][tid] += sharedSH[3][tid + stride];
            sharedWeight[tid] += sharedWeight[tid + stride];
            sharedLum[tid] += sharedLum[tid + stride];
            sharedLumSq[tid] += sharedLumSq[tid + stride];
            sharedHit[tid] += sharedHit[tid + stride];
            sharedActive[tid] += sharedActive[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (constants_sh2CheckMode > 0u)
    {
        bool _935 = constants_tier3Mode > 0u;
        bool _941;
        if (_935)
        {
            _941 = constants_tierRaysMode > 0u;
        }
        else
        {
            _941 = _935;
        }
        float _942;
        if (_941)
        {
            _942 = max(sharedActive[0], 1.0f);
        }
        else
        {
            _942 = float(constants_raysPerProbe);
        }
        float effRaysC = _942;
        float sumScaleC = 12.56637096405029296875f / effRaysC;
        float3 param_19 = sharedRayDir[tid];
        float4 Yc = shBasis(param_19);
        float3 recon = ((((sharedSH[0][0] * Yc.x) + (sharedSH[1][0] * Yc.y)) + (sharedSH[2][0] * Yc.z)) + (sharedSH[3][0] * Yc.w)) * sumScaleC;
        float dLumRaw = abs(dot(recon, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)) - dot(sharedRayRad[tid], float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)));
        bool _1000 = isnan(dLumRaw);
        bool _1006;
        if (!_1000)
        {
            _1006 = isinf(dLumRaw);
        }
        else
        {
            _1006 = _1000;
        }
        float dLum = _1006 ? 0.0f : dLumRaw;
        float3 _1014;
        if (sharedActive[tid] > 0.0f)
        {
            _1014 = float3(dLum, 1.0f, 0.0f);
        }
        else
        {
            _1014 = 0.0f.xxx;
        }
        sharedErrSum[tid] = _1014;
        GroupMemoryBarrierWithGroupSync();
        for (uint stride_1 = 32u; stride_1 > 0u; stride_1 = stride_1 >> 1u)
        {
            if (tid < stride_1)
            {
                sharedErrSum[tid] += sharedErrSum[tid + stride_1];
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (tid == 0u)
    {
        uint base_1 = group_id * 4u;
        float hitCount = sharedHit[0];
        float totalWeight = sharedWeight[0];
        float activeCount = sharedActive[0];
        if ((totalWeight > 0.0f) && (activeCount > 0.0f))
        {
            float effRays = float(constants_raysPerProbe);
            bool _1076 = constants_tier3Mode > 0u;
            bool _1082;
            if (_1076)
            {
                _1082 = constants_tierRaysMode > 0u;
            }
            else
            {
                _1082 = _1076;
            }
            if (_1082)
            {
                effRays = max(activeCount, 1.0f);
            }
            float sumScale = 12.56637096405029296875f / effRays;
            float hitRatio = hitCount / effRays;
            float _1097;
            if (activeCount > 0.0f)
            {
                _1097 = sharedLum[0] / activeCount;
            }
            else
            {
                _1097 = 0.0f;
            }
            float avgLum = _1097;
            float _1109;
            if (activeCount > 0.0f)
            {
                _1109 = sharedLumSq[0] / activeCount;
            }
            else
            {
                _1109 = 0.0f;
            }
            float avgLumSq = _1109;
            float lumVariance = max(avgLumSq - (avgLum * avgLum), 0.0f);
            float radianceConsistency = lerp(0.550000011920928955078125f, 1.0f, exp((-lumVariance) * 4.0f));
            float confidence = lerp(constants_skyConfFloor, 1.0f, hitRatio) * radianceConsistency;
            probeSHOut.Store4((base_1 + 0u) * 16 + 0, asuint(float4(sharedSH[0][0] * sumScale, confidence)));
            probeSHOut.Store4((base_1 + 1u) * 16 + 0, asuint(float4(sharedSH[1][0] * sumScale, 1.0f)));
            probeSHOut.Store4((base_1 + 2u) * 16 + 0, asuint(float4(sharedSH[2][0] * sumScale, 1.0f)));
            probeSHOut.Store4((base_1 + 3u) * 16 + 0, asuint(float4(sharedSH[3][0] * sumScale, 1.0f)));
        }
        else
        {
            probeSHOut.Store4((base_1 + 0u) * 16 + 0, asuint(0.0f.xxxx));
            probeSHOut.Store4((base_1 + 1u) * 16 + 0, asuint(0.0f.xxxx));
            probeSHOut.Store4((base_1 + 2u) * 16 + 0, asuint(0.0f.xxxx));
            probeSHOut.Store4((base_1 + 3u) * 16 + 0, asuint(0.0f.xxxx));
        }
        if (constants_sh2CheckMode > 0u)
        {
            sh2Check.Store4(group_id * 16 + 0, asuint(float4(sharedErrSum[0].x, sharedErrSum[0].y, sharedActive[0], 0.0f)));
        }
        float activeC = sharedActive[0];
        float hitC = sharedHit[0];
        float missC = max(activeC - hitC, 0.0f);
        float denom = max(activeC, 1.0f);
        probeOpenSky.Store4(group_id * 16 + 0, asuint(float4(missC / denom, hitC / denom, 0.0f, 1.0f)));
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
