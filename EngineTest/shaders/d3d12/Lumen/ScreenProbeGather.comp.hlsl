static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

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

float4 sampleBilinearRGBA(Texture2D<float4> tex, float2 uv)
{
    uint _62_dummy_parameter;
    float2 size = float2(int2(spvTextureSize(tex, uint(0), _62_dummy_parameter)));
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

float3 evalSHIrradiance(float3 sh0, float3 sh1, float3 sh2, float3 sh3, float3 N)
{
    return (((sh0 * 0.88622760772705078125f) + (sh1 * ((-1.02332770824432373046875f) * N.y))) + (sh2 * (1.02332770824432373046875f * N.z))) + (sh3 * ((-1.02332770824432373046875f) * N.x));
}

void comp_main()
{
    uint2 tid = gl_GlobalInvocationID.xy;
    uint renderW = uint(global_trace_params.z);
    uint renderH = uint(global_trace_params.w);
    bool _188 = tid.x >= renderW;
    bool _196;
    if (!_188)
    {
        _196 = tid.y >= renderH;
    }
    else
    {
        _196 = _188;
    }
    if (_196)
    {
        return;
    }
    uint gridW = uint(global_grid_params.x);
    uint gridH = uint(global_grid_params.y);
    float downsample = global_grid_params.z;
    float2 invRes = 1.0f.xx / float2(float(renderW), float(renderH));
    float2 pixelUV = (float2(tid) + 0.5f.xx) * invRes;
    float2 param = pixelUV;
    float depth = sampleBilinearRGBA(depthTexture, param).x;
    if ((depth <= 9.9999997473787516355514526367188e-05f) || (depth >= 0.999000012874603271484375f))
    {
        outputTexture[int2(tid)] = 0.0f.xxxx;
        return;
    }
    float2 param_1 = pixelUV;
    float4 normalEncoded = sampleBilinearRGBA(normalTexture, param_1);
    float3 surfaceNormal = normalize((normalEncoded.xyz * 2.0f) - 1.0f.xxx);
    float2 ndc = float2((pixelUV.x * 2.0f) - 1.0f, 1.0f - (pixelUV.y * 2.0f));
    float4 receiverH = mul(float4(ndc, depth, 1.0f), global_inv_view_projection);
    float3 receiverPos = receiverH.xyz / receiverH.w.xxx;
    float4 footprintH = mul(float4(ndc + float2((2.0f * downsample) / float(renderW), 0.0f), depth, 1.0f), global_inv_view_projection);
    float planeSigma = max(0.0199999995529651641845703125f, distance(receiverPos, footprintH.xyz / footprintH.w.xxx) * 0.25f);
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
        bool _386 = npc.x < 0;
        bool _393;
        if (!_386)
        {
            _393 = npc.y < 0;
        }
        else
        {
            _393 = _386;
        }
        bool _402;
        if (!_393)
        {
            _402 = npc.x >= int(gridW);
        }
        else
        {
            _402 = _393;
        }
        bool _411;
        if (!_402)
        {
            _411 = npc.y >= int(gridH);
        }
        else
        {
            _411 = _402;
        }
        if (_411)
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
            float4 posData = asfloat(probePositions.Load4(candIdx * 16 + 0));
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
        if (bestGeomW < 9.9999997473787516355514526367188e-06f)
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
        float3 sh0 = asfloat(probeSH.Load4((shBase + 0u) * 16 + 0)).xyz;
        float3 sh1 = asfloat(probeSH.Load4((shBase + 1u) * 16 + 0)).xyz;
        float3 sh2 = asfloat(probeSH.Load4((shBase + 2u) * 16 + 0)).xyz;
        float3 sh3 = asfloat(probeSH.Load4((shBase + 3u) * 16 + 0)).xyz;
        float3 param_2 = sh0;
        float3 param_3 = sh1;
        float3 param_4 = sh2;
        float3 param_5 = sh3;
        float3 param_6 = surfaceNormal;
        float3 irr = evalSHIrradiance(param_2, param_3, param_4, param_5, param_6);
        irr = max(irr, 0.0f.xxx);
        float baseWeight = (spatialWeight * geometryWeight_1) * sh0w;
        fallbackIrradianceSum += (irr * baseWeight);
        fallbackConfidenceOut += (sh0w * baseWeight);
        fallbackWeightSum += baseWeight;
        float normalWeight = 1.0f;
        float4 probeNormData = asfloat(probeNormals.Load4(probeIdx * 16 + 0));
        if (probeNormData.w > 0.0f)
        {
            float3 probeN = normalize(probeNormData.xyz);
            float nDot = dot(surfaceNormal, probeN);
            normalWeight = lerp(0.1500000059604644775390625f, 1.0f, smoothstep(-0.25f, 0.800000011920928955078125f, nDot));
        }
        float w = baseWeight * normalWeight;
        irradianceSum += (irr * w);
        confidenceOut += (sh0w * w);
        weightSum += w;
    }
    if ((weightSum <= 9.9999999747524270787835121154785e-07f) && (fallbackWeightSum > 9.9999999747524270787835121154785e-07f))
    {
        irradianceSum = fallbackIrradianceSum;
        confidenceOut = fallbackConfidenceOut;
        weightSum = fallbackWeightSum;
    }
    if (weightSum <= 9.9999999747524270787835121154785e-07f)
    {
        outputTexture[int2(tid)] = 0.0f.xxxx;
        return;
    }
    float3 finalIrr = irradianceSum / weightSum.xxx;
    float finalConf = confidenceOut / weightSum;
    outputTexture[int2(tid)] = float4(max(finalIrr, 0.0f.xxx), finalConf);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
