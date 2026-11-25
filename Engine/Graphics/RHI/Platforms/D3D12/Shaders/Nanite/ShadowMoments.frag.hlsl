// Vulkan reference: Nanite/ShadowMoments.frag (main).
static float4 fragCoord;

struct StageInput
{
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float2 outMoments : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    fragCoord = input.fragCoord;
    fragCoord.w = 1.0 / fragCoord.w;
    float z = fragCoord.z;
    output.outMoments = float2(z, z * z);
    return output;
}
