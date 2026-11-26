// Vulkan reference: BlurPass.spv (blurCS).
#include "Common/TextureDimensions.hlsli"

struct BlurParams
{
    uint textureWidth;
    uint textureHeight;
    int blurRadius;
    float sigma;
    uint arrayLayer;
    uint direction;
    uint _pad0;
    uint _pad1;
};

cbuffer params : register(b2, space0)
{
    BlurParams params_1_m0 : packoffset(c0);
};

Texture2D<float4> inputTex : register(t0, space0);
RWTexture2D<float4> outputTex : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    bool _43 = false;
    float4 sum = 0.0f.xxxx;

    uint2 _50 = textureDimensions(inputTex, 0u);
    if (!(input.dispatchThreadID.x >= _50.x))
    {
        _43 = input.dispatchThreadID.y >= _50.y;
    }
    else
    {
        _43 = true;
    }
    if (_43)
    {
        return;
    }
    uint2 _66 = uint2(input.dispatchThreadID.xy);
    bool2 _75 = (params_1_m0.direction == 1u).xx;
    int2 _73 = int2(_75.x ? int2(0, 1).x : int2(1, 0).x, _75.y ? int2(0, 1).y : int2(1, 0).y);
    sum = inputTex.Load(int3(_66, 0)) * 0.22702703f;
    sum += (inputTex.Load(int3(_66 + uint2(_73), 0)) * 0.31621623f);
    sum += (inputTex.Load(int3(_66 - uint2(_73), 0)) * 0.31621623f);
    sum += (inputTex.Load(int3(_66 + (uint2(_73) * uint2(2u, 2u)), 0)) * 0.07027027f);
    sum += (inputTex.Load(int3(_66 - (uint2(_73) * uint2(2u, 2u)), 0)) * 0.07027027f);
    outputTex[_66] = sum;
}
