// Vulkan reference: Lumen/ScreenProbeGather.comp (screen_probe_gather).
#include "../Common/TextureDimensions.hlsli"

cbuffer GlobalBlock : register(b5, space0)
{
    row_major float4x4 global_view_projection : packoffset(c0);
    row_major float4x4 global_inv_view_projection : packoffset(c4);
    float4 global_camera_position : packoffset(c8);
    float4 global_grid_params : packoffset(c9);
    float4 global_trace_params : packoffset(c10);
    float4 global_sdf_origins[3] : packoffset(c11);
    float4 global_sdf_voxel_sizes[3] : packoffset(c14);
    float4 global_sdf_extents[3] : packoffset(c17);
    uint4 global_sdf_resolutions : packoffset(c20);
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
};

ByteAddressBuffer probePositions : register(t3, space0);
ByteAddressBuffer probeSH : register(t4, space0);
ByteAddressBuffer probeNormals : register(t6, space0);
Texture2D<float4> depthTexture : register(t0, space0);
RWTexture2D<float4> outputTexture : register(u2, space0);
Texture2D<float4> normalTexture : register(t1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float4 sampleBilinearRGBA(Texture2D<float4> tex, float2 uv)
{

    float2 size = float2(int2(textureDimensions(tex, 0u)));
    float2 px = (uv * size) - 0.5f.xx;
    int2 p0 = int2(floor(px));
    float2 f = frac(px);
    int2 c00 = clamp(p0, int2(0, 0), int2(size) - int2(1, 1));
    int2 c10 = clamp(p0 + int2(1, 0), int2(0, 0), int2(size) - int2(1, 1));
    int2 c01 = clamp(p0 + int2(0, 1), int2(0, 0), int2(size) - int2(1, 1));
    int2 c11 = clamp(p0 + int2(1, 1), int2(0, 0), int2(size) - int2(1, 1));
    float4 s00 = tex.Load(int3(c00, 0));
    float4 s10 = tex.Load(int3(c10, 0));
    float4 s01 = tex.Load(int3(c01, 0));
    float4 s11 = tex.Load(int3(c11, 0));
    float4 a = lerp(s00, s10, f.x.xxxx);
    float4 b = lerp(s01, s11, f.x.xxxx);
    return lerp(a, b, f.y.xxxx);
}

float3 evalSHIrradiance(float3 sh0, float3 sh1, float3 sh2, float3 sh3, float3 N) {
    return sh0 * (0.282095 * 3.14159265)
         + sh1 * (-0.488603 * 2.09439510 * N.y)
         + sh2 * ( 0.488603 * 2.09439510 * N.z)
         + sh3 * (-0.488603 * 2.09439510 * N.x);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 tid = input.dispatchThreadID.xy;
    uint renderW = uint(global_trace_params.z);
    uint renderH = uint(global_trace_params.w);
    if (((tid.x >= renderW) || (tid.y >= renderH)))
    {
        return;
    }
    uint gridW = uint(global_grid_params.x);
    uint gridH = uint(global_grid_params.y);
    float downsample = global_grid_params.z;
    float2 invRes = 1.0f.xx / float2(float(renderW), float(renderH));
    float2 pixelUV = (float2(tid) + 0.5f.xx) * invRes;
    float depth = sampleBilinearRGBA(depthTexture, pixelUV).x;
    if ((depth <= 0.0001f) || (depth >= 0.999f))
    {
        outputTexture[int2(tid)] = 0.0f.xxxx;
        return;
    }
    float4 normalEncoded = sampleBilinearRGBA(normalTexture, pixelUV);
    float3 surfaceNormal = normalize((normalEncoded.xyz * 2.0f) - 1.0f.xxx);
    float2 ndc = float2((pixelUV.x * 2.0f) - 1.0f, 1.0f - (pixelUV.y * 2.0f));
    float4 receiverH = mul(float4(ndc, depth, 1.0f), global_inv_view_projection);
    float3 receiverPos = receiverH.xyz / receiverH.w.xxx;
    float4 footprintH = mul(float4(ndc + float2((2.0f * downsample) / float(renderW), 0.0f), depth, 1.0f), global_inv_view_projection);
    float planeSigma = max(0.02f, distance(receiverPos, footprintH.xyz / footprintH.w.xxx) * 0.25f);
    float2 probeCoord = ((float2(tid) + 0.5f.xx) / downsample.xx) - 0.5f.xx;
    int2 baseProbe = int2(floor(probeCoord));
    float3 irradianceSum = 0.0f.xxx;
    float confidenceOut = 0.0f;
    float weightSum = 0.0f;
    float3 fallbackIrradianceSum = 0.0f.xxx;
    float fallbackConfidenceOut = 0.0f;
    float fallbackWeightSum = 0.0f;
    for (int i = 0; i < 16; i++)
    {
        int2 npc = baseProbe + int2((i % 4) - 1, (i / 4) - 1);
        float2 tent = max(0.0f.xx, 1.0f.xx - (abs(float2(npc) - probeCoord) * 0.5f));
        float spatialWeight = tent.x * tent.y;
        if (spatialWeight <= 0.0f)
        {
            continue;
        }
        if (((((npc.x < 0) || (npc.y < 0)) || (npc.x >= int(gridW))) || (npc.y >= int(gridH))))
        {
            continue;
        }
        uint slotsPerTile = (global_tier_params.x > 0.5f) ? 4u : 1u;
        uint tileIdx = (uint(npc.y) * gridW) + uint(npc.x);
        uint bestSlot = 4294967295u;
        float bestGeomW = -1.0f;
        float4 bestPos = 0.0f.xxxx;
        for (uint s = 0u; s < slotsPerTile; s++)
        {
            uint candIdx = (tileIdx * slotsPerTile) + s;
            float4 posData = asfloat(probePositions.Load4(candIdx * 16));
            if (posData.w <= 0.0f)
            {
                continue;
            }
            float3 surfaceDelta = posData.xyz - receiverPos;
            float planeDistance = abs(dot(surfaceDelta, surfaceNormal));
            float geometryWeight = exp((((-0.5f) * planeDistance) * planeDistance) / (planeSigma * planeSigma));
            if (geometryWeight > bestGeomW)
            {
                bestGeomW = geometryWeight;
                bestSlot = candIdx;
                bestPos = posData;
            }
        }
        if (bestSlot == 4294967295u)
        {
            continue;
        }
        if (bestGeomW < 1e-05f)
        {
            continue;
        }
        uint probeIdx = bestSlot;
        float4 posData_1 = bestPos;
        float geometryWeight_1 = bestGeomW;
        uint shBase = probeIdx * 4u;
        float sh0w = asfloat(probeSH.Load((shBase + 0u) * 16 + 12));
        if (sh0w <= 0.0f)
        {
            continue;
        }
        float3 sh0 = asfloat(probeSH.Load4((shBase + 0u) * 16)).xyz;
        float3 sh1 = asfloat(probeSH.Load4((shBase + 1u) * 16)).xyz;
        float3 sh2 = asfloat(probeSH.Load4((shBase + 2u) * 16)).xyz;
        float3 sh3 = asfloat(probeSH.Load4((shBase + 3u) * 16)).xyz;
        float3 param_2 = sh0;
        float3 param_3 = sh1;
        float3 param_4 = sh2;
        float3 param_5 = sh3;
        float3 irr = evalSHIrradiance(param_2, param_3, param_4, param_5, surfaceNormal);
        irr = max(irr, 0.0f.xxx);
        float baseWeight = (spatialWeight * geometryWeight_1) * sh0w;
        fallbackIrradianceSum += (irr * baseWeight);
        fallbackConfidenceOut += (sh0w * baseWeight);
        fallbackWeightSum += baseWeight;
        float normalWeight = 1.0f;
        float4 probeNormData = asfloat(probeNormals.Load4(probeIdx * 16));
        if (probeNormData.w > 0.0f)
        {
            float3 probeN = normalize(probeNormData.xyz);
            float nDot = dot(surfaceNormal, probeN);
            normalWeight = lerp(0.15f, 1.0f, smoothstep(-0.25f, 0.8f, nDot));
        }
        float w = baseWeight * normalWeight;
        irradianceSum += (irr * w);
        confidenceOut += (sh0w * w);
        weightSum += w;
    }
    if ((weightSum <= 1e-06f) && (fallbackWeightSum > 1e-06f))
    {
        irradianceSum = fallbackIrradianceSum;
        confidenceOut = fallbackConfidenceOut;
        weightSum = fallbackWeightSum;
    }
    if (weightSum <= 1e-06f)
    {
        outputTexture[int2(tid)] = 0.0f.xxxx;
        return;
    }
    float3 finalIrr = irradianceSum / weightSum.xxx;
    float finalConf = confidenceOut / weightSum;
    outputTexture[int2(tid)] = float4(max(finalIrr, 0.0f.xxx), finalConf);
}
