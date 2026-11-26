// Vulkan reference: Forward/Line.frag (main).
struct StageOutput
{
    float4 outFragColor : SV_Target0;
};

StageOutput main()
{
    StageOutput output = (StageOutput)0;
    output.outFragColor = float4(0.2f, 1.0f, 0.4f, 1.0f);
    return output;
}
