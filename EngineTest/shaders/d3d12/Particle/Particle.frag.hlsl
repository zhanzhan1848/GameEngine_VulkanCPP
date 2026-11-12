Texture2D<float4> particle_texture : register(t3, space0);
SamplerState _particle_texture_sampler : register(s3, space0);

static float4 outFragColor;
static float4 inColor;
static float2 inUV;
static float inAge;

struct SPIRV_Cross_Input
{
    float4 inColor : TEXCOORD0;
    float2 inUV : TEXCOORD1;
    float inAge : TEXCOORD2;
};

struct SPIRV_Cross_Output
{
    float4 outFragColor : SV_Target0;
};

void frag_main()
{
    outFragColor = float4(1.0f, 0.0f, 1.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inColor = stage_input.inColor;
    inUV = stage_input.inUV;
    inAge = stage_input.inAge;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outFragColor = outFragColor;
    return stage_output;
}
