// Vulkan reference: PFX/ParticleFXLight.frag (main).
struct StageInput
{
    float4 in_color : TEXCOORD0;
    float2 in_uv : TEXCOORD1;
};

struct StageOutput
{
    float4 out_color : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float2 c = (input.in_uv - 0.5f.xx) * 2.0f;
    float r2 = dot(c, c);
    float falloff = clamp(1.0f - r2, 0.0f, 1.0f);
    falloff *= falloff;
    output.out_color = float4(input.in_color.xyz * falloff, falloff);
    return output;
}
