// Vulkan reference: Lumen/ScreenProbeDenoise.comp (screen_probe_denoise).
#include "../Common/TextureDimensions.hlsli"

cbuffer DenoiseParamsBlock : register(b4, space0)
{
    float params_sigma_depth : packoffset(c0);
    float params_sigma_normal : packoffset(c0.y);
    float params_sigma_spatial : packoffset(c0.z);
    float params_pad1 : packoffset(c0.w);
    uint params_kernel_radius : packoffset(c1);
    uint params_render_width : packoffset(c1.y);
    uint params_render_height : packoffset(c1.z);
    uint params_pad2 : packoffset(c1.w);
};

Texture2D<float4> giInput : register(t0, space0);
Texture2D<float4> depthTexture : register(t2, space0);
Texture2D<float4> normalTexture : register(t1, space0);
RWTexture2D<float4> giOutput : register(u3, space0);

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

float3 sampleBilinearRGB(Texture2D<float4> tex, float2 uv)
{

    float2 size = float2(int2(textureDimensions(tex, 0u)));
    float2 px = (uv * size) - 0.5f.xx;
    int2 p0 = int2(floor(px));
    float2 f = frac(px);
    int2 c00 = clamp(p0, int2(0, 0), int2(size) - int2(1, 1));
    int2 c10 = clamp(p0 + int2(1, 0), int2(0, 0), int2(size) - int2(1, 1));
    int2 c01 = clamp(p0 + int2(0, 1), int2(0, 0), int2(size) - int2(1, 1));
    int2 c11 = clamp(p0 + int2(1, 1), int2(0, 0), int2(size) - int2(1, 1));
    float3 s00 = tex.Load(int3(c00, 0)).xyz;
    float3 s10 = tex.Load(int3(c10, 0)).xyz;
    float3 s01 = tex.Load(int3(c01, 0)).xyz;
    float3 s11 = tex.Load(int3(c11, 0)).xyz;
    float3 a = lerp(s00, s10, f.x.xxx);
    float3 b = lerp(s01, s11, f.x.xxx);
    return lerp(a, b, f.y.xxx);
}

float3 decodeNormal(float4 encoded) {
    return normalize(encoded.xyz * 2.0 - 1.0);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 pixel_pos = input.dispatchThreadID.xy;
    uint width = params_render_width;
    uint height = params_render_height;
    if (((pixel_pos.x >= width) || (pixel_pos.y >= height)))
    {
        return;
    }
    float2 invRes = 1.0f.xx / float2(float(width), float(height));
    float2 centerUV = (float2(pixel_pos) + 0.5f.xx) * invRes;
    float4 centerGI = sampleBilinearRGBA(giInput, centerUV);
    float centerDepth = depthTexture.Load(int3(int2(pixel_pos), 0)).x;
    float4 param_2 = float4(sampleBilinearRGB(normalTexture, centerUV), 0.0f);
    float3 centerNormal = decodeNormal(param_2);
    if ((centerDepth <= 0.0001f) || (centerDepth >= 0.999f))
    {
        giOutput[int2(pixel_pos)] = 0.0f.xxxx;
        return;
    }
    uint radius = min(params_kernel_radius, 2u);
    float3 filteredGI = 0.0f.xxx;
    float filteredConfidence = 0.0f;
    float totalWeight = 0.0f;
    float centerConfidence = clamp(centerGI.w, 0.0f, 1.0f);
    int _341 = -int(radius);
    for (int dy = _341; dy <= int(radius); dy++)
    {
        int _354 = -int(radius);
        for (int dx = _354; dx <= int(radius); dx++)
        {
            int2 samplePos = int2(pixel_pos) + int2(dx, dy);
            if (((((samplePos.x < 0) || (samplePos.y < 0)) || (samplePos.x >= int(width))) || (samplePos.y >= int(height))))
            {
                continue;
            }
            float2 sampleUV = (float2(samplePos) + 0.5f.xx) * invRes;
            float4 sampleGI = sampleBilinearRGBA(giInput, sampleUV);
            float sampleDepth = depthTexture.Load(int3(samplePos, 0)).x;
            float4 param_5 = float4(sampleBilinearRGB(normalTexture, sampleUV), 0.0f);
            float3 sampleNormal = decodeNormal(param_5);
            float depthDiff = abs(centerDepth - sampleDepth);
            float w_depth = exp((-depthDiff) * params_sigma_depth);
            float NdotN = max(0.0f, dot(centerNormal, sampleNormal));
            float w_normal = pow(NdotN, params_sigma_normal);
            float spatialDist2 = float((dx * dx) + (dy * dy));
            float w_spatial = exp((-spatialDist2) / ((2.0f * params_sigma_spatial) * params_sigma_spatial));
            float sampleConfidence = clamp(sampleGI.w, 0.0f, 1.0f);
            float confidenceWeight = lerp(max(sampleConfidence, 0.05f), 1.0f, centerConfidence);
            float weight = ((w_depth * w_normal) * w_spatial) * confidenceWeight;
            filteredGI += (sampleGI.xyz * weight);
            filteredConfidence += (sampleConfidence * weight);
            totalWeight += weight;
        }
    }
    if (totalWeight > 1e-06f)
    {
        filteredGI /= totalWeight.xxx;
        filteredConfidence /= totalWeight;
    }
    else
    {
        filteredGI = centerGI.xyz;
        filteredConfidence = centerConfidence;
    }
    giOutput[int2(pixel_pos)] = float4(filteredGI, max(centerConfidence, filteredConfidence));
}
