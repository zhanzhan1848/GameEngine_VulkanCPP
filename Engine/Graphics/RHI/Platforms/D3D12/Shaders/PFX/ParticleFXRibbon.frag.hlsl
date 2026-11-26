// Vulkan reference: PFX/ParticleFXRibbon.frag (main).
struct StageInput
{
    float2 in_uv : TEXCOORD0;
    float4 in_color : TEXCOORD1;
};

struct StageOutput
{
    float4 out_color : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float edge = 1.0f - (((input.in_uv.x - 0.5f) * (input.in_uv.x - 0.5f)) * 4.0f);
    float taper = edge * edge;
    float along = 0.25f + (0.75f * input.in_uv.y);
    output.out_color = float4(input.in_color.xyz * along, input.in_color.w * taper);
    return output;
}
