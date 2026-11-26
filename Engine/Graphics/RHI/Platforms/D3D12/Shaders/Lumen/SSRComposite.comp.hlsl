// Vulkan reference: Lumen/SSRComposite.comp (main).
cbuffer SSRCompositeParams : register(b3, space0)
{
    uint params_screenWidth : packoffset(c0);
    uint params_screenHeight : packoffset(c0.y);
    uint params_halfWidth : packoffset(c0.z);
    uint params_halfHeight : packoffset(c0.w);
    float params_maxRoughness : packoffset(c1);
    float params_reflectionStrength : packoffset(c1.y);
    float params_pad0 : packoffset(c1.z);
    float params_pad1 : packoffset(c1.w);
};

Texture2D<float4> ssrTex : register(t0, space0);
Texture2D<float4> ormTex : register(t1, space0);
RWTexture2D<float4> outTex : register(u2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float4 sampleBilinear(Texture2D<float4> tex, float2 uv, int2 dims)
{
    float2 coord = (uv * float2(dims)) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 frac_ = frac(coord);
    int2 maxCoord = dims - int2(1, 1);
    int2 c00 = clamp(base, int2(0, 0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1), int2(0, 0), maxCoord);
    float4 h00 = tex.Load(int3(c00, 0));
    float4 h10 = tex.Load(int3(c10, 0));
    float4 h01 = tex.Load(int3(c01, 0));
    float4 h11 = tex.Load(int3(c11, 0));
    float fx = clamp(frac_.x, 0.0f, 1.0f);
    float fy = clamp(frac_.y, 0.0f, 1.0f);
    return ((((h00 * (1.0f - fx)) * (1.0f - fy)) + ((h10 * fx) * (1.0f - fy))) + ((h01 * (1.0f - fx)) * fy)) + ((h11 * fx) * fy);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint3 gid = input.dispatchThreadID;
    if (((gid.x >= params_screenWidth) || (gid.y >= params_screenHeight)))
    {
        return;
    }
    int2 pixel = int2(gid.xy);
    float2 fullUV = (float2(pixel) + 0.5f.xx) / float2(float(params_screenWidth), float(params_screenHeight));
    int2 halfDims = int2(int(params_halfWidth), int(params_halfHeight));
    float2 param = fullUV;
    int2 param_1 = halfDims;
    float4 ssr = sampleBilinear(ssrTex, param, param_1);
    float roughness = ormTex.Load(int3(pixel, 0)).y;
    float roughFade = 1.0f - smoothstep(params_maxRoughness * 0.5f, params_maxRoughness, roughness);
    float strength = (ssr.w * roughFade) * params_reflectionStrength;
    float3 reflection = ssr.xyz * strength;
    outTex[pixel] = float4(reflection, strength);
}
