// Vulkan reference: Forward/M6ProbeAccum.comp (main).
cbuffer AccumParams : register(b4, space0)
{
    float4 params_flags : packoffset(c0);
};

RWTexture3D<float4> newGrid : register(u0, space0);
Texture3D<float4> histTexture : register(t1, space0);
SamplerState histSampler : register(s2, space0);
RWTexture3D<float4> outGrid : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(4, 4, 4)]
void main(StageInput input)
{
    int3 c = int3(input.dispatchThreadID);
    float4 newV = newGrid[c];
    float4 _38;
    if (params_flags.x > 0.5f)
    {
        _38 = 0.0f.xxxx;
    }
    else
    {
        _38 = histTexture.Load(int4(c, 0));
    }
    float4 histV = _38;
    outGrid[c] = (histV * 0.9f) + (newV * 0.1f);
}
