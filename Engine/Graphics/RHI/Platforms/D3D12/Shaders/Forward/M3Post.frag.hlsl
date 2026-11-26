// Vulkan reference: Forward/M3Post.frag (main).
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

struct StageInput
{
    float2 inUV : TEXCOORD0;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float3 ACESFilm(float3 x)
{
    return clamp((x * ((params_acesA.xxx * x) + params_acesB.xxx)) / ((x * ((params_acesC.xxx * x) + params_acesD.xxx)) + params_acesE.xxx), 0.0f.xxx, 1.0f.xxx);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 hdr = hdrTexture.Sample(hdrSampler, input.inUV).xyz;
    float3 result = ACESFilm(hdr * params_exposure);
    output.outColor = float4(result, 1.0f);
    return output;
}
