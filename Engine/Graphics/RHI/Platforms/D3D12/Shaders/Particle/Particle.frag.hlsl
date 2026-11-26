// Vulkan reference: Particle/Particle.frag.spv (main).
Texture2D<float4> particle_texture : register(t3, space0);
SamplerState _particle_texture_sampler : register(s3, space0);

struct StageInput
{
    float4 inColor : TEXCOORD0;
    float2 inUV : TEXCOORD1;
    float inAge : TEXCOORD2;
};

struct StageOutput
{
    float4 outFragColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.outFragColor = float4(1.0f, 0.0f, 1.0f, 1.0f);
    return output;
}
