static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

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
    uint _145_dummy_parameter;
    float2 size = float2(int2(spvTextureSize(tex, uint(0), _145_dummy_parameter)));
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
    uint _43_dummy_parameter;
    float2 size = float2(int2(spvTextureSize(tex, uint(0), _43_dummy_parameter)));
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

float3 decodeNormal(float4 encoded)
{
    return normalize((encoded.xyz * 2.0f) - 1.0f.xxx);
}

void comp_main()
{
    uint2 pixel_pos = gl_GlobalInvocationID.xy;
    uint width = params_render_width;
    uint height = params_render_height;
    bool _254 = pixel_pos.x >= width;
    bool _262;
    if (!_254)
    {
        _262 = pixel_pos.y >= height;
    }
    else
    {
        _262 = _254;
    }
    if (_262)
    {
        return;
    }
    float2 invRes = 1.0f.xx / float2(float(width), float(height));
    float2 centerUV = (float2(pixel_pos) + 0.5f.xx) * invRes;
    float2 param = centerUV;
    float4 centerGI = sampleBilinearRGBA(giInput, param);
    float centerDepth = depthTexture.Load(int3(int2(pixel_pos), 0)).x;
    float2 param_1 = centerUV;
    float4 param_2 = float4(sampleBilinearRGB(normalTexture, param_1), 0.0f);
    float3 centerNormal = decodeNormal(param_2);
    if ((centerDepth <= 9.9999997473787516355514526367188e-05f) || (centerDepth >= 0.999000012874603271484375f))
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
            bool _373 = samplePos.x < 0;
            bool _380;
            if (!_373)
            {
                _380 = samplePos.y < 0;
            }
            else
            {
                _380 = _373;
            }
            bool _389;
            if (!_380)
            {
                _389 = samplePos.x >= int(width);
            }
            else
            {
                _389 = _380;
            }
            bool _398;
            if (!_389)
            {
                _398 = samplePos.y >= int(height);
            }
            else
            {
                _398 = _389;
            }
            if (_398)
            {
                continue;
            }
            float2 sampleUV = (float2(samplePos) + 0.5f.xx) * invRes;
            float2 param_3 = sampleUV;
            float4 sampleGI = sampleBilinearRGBA(giInput, param_3);
            float sampleDepth = depthTexture.Load(int3(samplePos, 0)).x;
            float2 param_4 = sampleUV;
            float4 param_5 = float4(sampleBilinearRGB(normalTexture, param_4), 0.0f);
            float3 sampleNormal = decodeNormal(param_5);
            float depthDiff = abs(centerDepth - sampleDepth);
            float w_depth = exp((-depthDiff) * params_sigma_depth);
            float NdotN = max(0.0f, dot(centerNormal, sampleNormal));
            float w_normal = pow(NdotN, params_sigma_normal);
            float spatialDist2 = float((dx * dx) + (dy * dy));
            float w_spatial = exp((-spatialDist2) / ((2.0f * params_sigma_spatial) * params_sigma_spatial));
            float sampleConfidence = clamp(sampleGI.w, 0.0f, 1.0f);
            float confidenceWeight = lerp(max(sampleConfidence, 0.0500000007450580596923828125f), 1.0f, centerConfidence);
            float weight = ((w_depth * w_normal) * w_spatial) * confidenceWeight;
            filteredGI += (sampleGI.xyz * weight);
            filteredConfidence += (sampleConfidence * weight);
            totalWeight += weight;
        }
    }
    if (totalWeight > 9.9999999747524270787835121154785e-07f)
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

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
