// Vulkan reference: Nanite/MirrorReflection.frag (main).
cbuffer ReflectionParams : register(b0, space0)
{
    row_major float4x4 reflected_view_projection : packoffset(c0);
    float4 light_dir : packoffset(c4);
    float4 light_color : packoffset(c5);
    float4 ambient : packoffset(c6);
};

Texture2DArray<float4> albedoTextures : register(t9, space0);
SamplerState albedoSampler : register(s10, space0);

struct StageInput
{
    float2 vUV : TEXCOORD0;
    float3 vNormalW : TEXCOORD1;
    float3 vAlbedoTint : TEXCOORD2;
    nointerpolation uint vAlbedoIdx : TEXCOORD3;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 albedo = albedoTextures.Sample(albedoSampler, float3(input.vUV, float(input.vAlbedoIdx))).xyz * input.vAlbedoTint;
    float NdotL = max(dot(normalize(input.vNormalW), normalize(light_dir.xyz)), 0.0f);
    float3 color = albedo * (ambient.xyz + (light_color.xyz * NdotL));
    output.outColor = float4(color, 1.0f);
    return output;
}
