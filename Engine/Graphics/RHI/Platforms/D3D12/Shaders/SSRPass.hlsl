// Vulkan reference: SSRPass.spv (ssrCS).
struct SSRParams
{
    row_major float4x4 viewProj;
    row_major float4x4 invViewProj;
    float screenWidth;
    float screenHeight;
    float _pad0;
    float _pad1;
};

cbuffer params : register(b4, space0)
{
    SSRParams params_1_m0 : packoffset(c0);
};

Texture2D<float4> colorTex : register(t0, space0);
Texture2D<float4> depthTex : register(t1, space0);
Texture2D<float4> normalTex : register(t2, space0);
RWTexture2D<float4> outputTex : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    bool _37 = false;
    uint2 _54 = uint2(uint(clamp(params_1_m0.screenWidth, 0.0f, 4294967040.0f)), uint(clamp(params_1_m0.screenHeight, 0.0f, 4294967040.0f)));
    if (!(input.dispatchThreadID.x >= _54.x))
    {
        _37 = input.dispatchThreadID.y >= _54.y;
    }
    else
    {
        _37 = true;
    }
    if (_37)
    {
        return;
    }
    uint2 _70 = uint2(input.dispatchThreadID.xy);
    outputTex[_70] = colorTex.Load(int3(_70, 0));
}
