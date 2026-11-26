// Vulkan reference: Debug/PassDebugViewSimple.frag (main).
Texture2D<float4> srcTex : register(t0, space0);
SamplerState inputSamp : register(s1, space0);

struct StageInput
{
    float2 inUv : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.outColor = float4(srcTex.Sample(inputSamp, input.inUv).xyz, 1.0f);
    return output;
}
