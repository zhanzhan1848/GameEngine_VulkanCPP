// Vulkan reference: PostProcess/Toon.comp (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer ToonConstantsUBO : register(b4, space0)
{
    float toon_edgeThreshold : packoffset(c0);
    float toon_colorLevels : packoffset(c0.y);
    float toon_pad0 : packoffset(c0.z);
    float toon_pad1 : packoffset(c0.w);
};

Texture2D<float4> inDepthTex : register(t1, space0);
RWTexture2D<unorm float4> outColorImg : register(u3, space0);
Texture2D<float4> inColorTex : register(t0, space0);
Texture2D<float4> inNormalTex : register(t2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float detectEdge(int2 pixel, int2 dims) {
    int2 c = clamp(pixel, (int2)(0), dims - (int2)(1));
    float d = inDepthTex.Load(int3(c, 0)).r;
    float dLeft  = inDepthTex.Load(int3(clamp(pixel + int2(-1, 0), (int2)(0), dims - (int2)(1)), 0)).r;
    float dRight = inDepthTex.Load(int3(clamp(pixel + int2(1, 0), (int2)(0), dims - (int2)(1)), 0)).r;
    float dUp    = inDepthTex.Load(int3(clamp(pixel + int2(0, -1), (int2)(0), dims - (int2)(1)), 0)).r;
    float dDown  = inDepthTex.Load(int3(clamp(pixel + int2(0, 1), (int2)(0), dims - (int2)(1)), 0)).r;

    return abs(d - dLeft) + abs(d - dRight) + abs(d - dUp) + abs(d - dDown);
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    int2 pixel = int2(input.dispatchThreadID.xy);

    int2 dims = int2(imageDimensions(outColorImg));
    if (((((pixel.x < 0) || (pixel.y < 0)) || (pixel.x >= dims.x)) || (pixel.y >= dims.y)))
    {
        return;
    }
    float4 color = inColorTex.Load(int3(pixel, 0));
    float3 quantized = floor(color.xyz * toon_colorLevels) / toon_colorLevels.xxx;
    int2 param = pixel;
    int2 param_1 = dims;
    float edge = detectEdge(param, param_1);
    float edgeFactor = (edge > toon_edgeThreshold) ? 0.0f : 1.0f;
    outColorImg[pixel] = float4(quantized * edgeFactor, color.w);
}
