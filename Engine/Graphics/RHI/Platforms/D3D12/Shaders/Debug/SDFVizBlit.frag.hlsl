// Vulkan reference: Debug/SDFVizBlit.frag (main).
Texture2D<float4> inputTex : register(t0, space0);
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
    float4 c = inputTex.Sample(inputSamp, input.inUv);
    output.outColor = c;
    return output;
}
