cbuffer PostParams : register(b0, space0)
{
    float params_exposure : packoffset(c0);
    float params_acesA : packoffset(c0.y);
    float params_acesB : packoffset(c0.z);
    float params_acesC : packoffset(c0.w);
    float params_acesD : packoffset(c1);
    float params_acesE : packoffset(c1.y);
    float params_pad0 : packoffset(c1.z);
    float params_pad1 : packoffset(c1.w);
};

Texture2D<float4> hdrTexture : register(t1, space0);
SamplerState hdrSampler : register(s2, space0);

static float2 inUV;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUV : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

float3 ACESFilm(float3 x)
{
    return clamp((x * ((params_acesA.xxx * x) + params_acesB.xxx)) / ((x * ((params_acesC.xxx * x) + params_acesD.xxx)) + params_acesE.xxx), 0.0f.xxx, 1.0f.xxx);
}

void frag_main()
{
    float3 hdr = hdrTexture.Sample(hdrSampler, inUV).xyz;
    float3 param = hdr * params_exposure;
    float3 result = ACESFilm(param);
    outColor = float4(result, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUV = stage_input.inUV;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
