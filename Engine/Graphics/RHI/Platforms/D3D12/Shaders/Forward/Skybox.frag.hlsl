// Vulkan reference: Forward/Skybox.frag (main).
TextureCube<float4> skyboxTex : register(t2, space0);
SamplerState skyboxSamp : register(s3, space0);

struct StageInput
{
    float3 inUv : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.outColor = skyboxTex.Sample(skyboxSamp, input.inUv);
    return output;
}
