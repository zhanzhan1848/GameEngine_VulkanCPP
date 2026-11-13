cbuffer GiGainUBO : register(b6, space0)
{
    float _98_giIndirectGain : packoffset(c0);
    float _98_fusionMode : packoffset(c0.y);
};

Texture2D<float4> ssgiColor : register(t0, space0);
SamplerState defaultSampler : register(s5, space0);
Texture2D<float4> ddgiColor : register(t1, space0);
Texture2D<float4> spgiColor : register(t2, space0);
Texture2D<float4> albedoTex : register(t3, space0);
Texture2D<float4> ssaoTex : register(t4, space0);

static float2 inUv;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float4 ssgi4 = ssgiColor.Sample(defaultSampler, inUv);
    float3 ddgi = ddgiColor.Sample(defaultSampler, inUv).xyz;
    float4 spgi4 = spgiColor.Sample(defaultSampler, inUv);
    float3 albedo = albedoTex.Sample(defaultSampler, inUv).xyz;
    float ssao = ssaoTex.Sample(defaultSampler, inUv).x;
    if (ssao <= 0.0f)
    {
        ssao = 1.0f;
    }
    float3 ssgi_irr = ssgi4.xyz;
    float ssgi_hit = ssgi4.w;
    float ssgi_hit_norm = clamp(ssgi_hit, 0.0f, 30.0f) / 30.0f;
    float ssgi_conf = clamp(1.0f - (ssgi_hit_norm * 0.699999988079071044921875f), 0.300000011920928955078125f, 1.0f);
    float spgi_w = clamp(spgi4.w, 0.0f, 0.89999997615814208984375f);
    albedo = clamp(albedo, 0.0f.xxx, 1.0f.xxx);
    float3 gi;
    float ssgiBlend;
    if (_98_fusionMode < 0.5f)
    {
        gi = lerp(ddgi, spgi4.xyz, spgi_w.xxx);
        ssgiBlend = lerp(0.25f, 0.1500000059604644775390625f, spgi_w);
    }
    else
    {
        if (_98_fusionMode < 1.5f)
        {
            gi = lerp(ddgi, spgi4.xyz, spgi_w.xxx);
            ssgiBlend = lerp(0.25f, 0.1500000059604644775390625f, spgi_w);
        }
        else
        {
            gi = ddgi;
            ssgiBlend = 0.25f;
        }
    }
    gi += ((ssgi_irr * ssgi_conf) * ssgiBlend);
    float3 indirect = ((albedo * gi) * _98_giIndirectGain) * 0.3183098733425140380859375f;
    outColor = float4(indirect, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUv = stage_input.inUv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
