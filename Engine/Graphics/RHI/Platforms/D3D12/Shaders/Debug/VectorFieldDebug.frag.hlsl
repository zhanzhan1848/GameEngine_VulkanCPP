// Vulkan reference: Debug/VectorFieldDebug.frag (main).
struct StageInput
{
    float3 vColor : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.outColor = float4(input.vColor, 1.0f);
    return output;
}
