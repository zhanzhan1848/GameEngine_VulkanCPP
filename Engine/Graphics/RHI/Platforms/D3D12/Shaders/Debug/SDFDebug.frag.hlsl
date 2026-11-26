// Vulkan reference: Debug/SDFDebug.frag (main).
cbuffer DebugUniforms : register(b1, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    float slice_depth : packoffset(c8);
    float mode : packoffset(c8.y);
    float4 pad0 : packoffset(c9);
};

Texture3D<float4> sdfTexture : register(t0, space0);
SamplerState linearSampler : register(s2, space0);

struct StageInput
{
    float2 vUV : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 uvw = float3(input.vUV.x, input.vUV.y, slice_depth);
    float dist = sdfTexture.Sample(linearSampler, uvw).x;
    float d = dist * 10.0f;
    float3 color;
    if (d < 0.0f)
    {
        color = lerp(float3(0.0f, 0.0f, 1.0f), float3(0.0f, 1.0f, 1.0f), (1.0f + d).xxx);
        if (d < (-1.0f))
        {
            color = float3(0.0f, 0.0f, 1.0f);
        }
    }
    else
    {
        color = lerp(float3(1.0f, 0.0f, 0.0f), float3(1.0f, 1.0f, 0.0f), d.xxx);
        if (d > 1.0f)
        {
            color = float3(1.0f, 1.0f, 0.0f);
        }
    }
    if (abs(dist) < 0.01f)
    {
        color = 1.0f.xxx;
    }
    output.outColor = float4(color, 0.8f);
    return output;
}
