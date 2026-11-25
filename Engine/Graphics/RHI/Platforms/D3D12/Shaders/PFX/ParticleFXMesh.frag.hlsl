// Vulkan reference: PFX/ParticleFXMesh.frag (main).
struct StageInput
{
    float3 in_normal : TEXCOORD0;
    float4 in_color : TEXCOORD1;
};

struct StageOutput
{
    float4 out_color : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.out_color = float4(input.in_color.xyz, 1.0f);
    return output;
}
