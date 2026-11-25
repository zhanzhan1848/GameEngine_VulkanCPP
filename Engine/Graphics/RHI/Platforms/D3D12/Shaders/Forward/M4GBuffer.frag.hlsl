// Vulkan reference: Forward/M4GBuffer.frag (main).
Texture2D<float4> albedoTexture : register(t0, space1);
SamplerState albedoSampler : register(s1, space1);

struct StageInput
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float4 inBaseColor : TEXCOORD3;
};

struct StageOutput
{
    float4 outAlbedo : SV_Target0;
    float2 outNormal : SV_Target1;
    float4 outMaterial : SV_Target2;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 albedo = albedoTexture.Sample(albedoSampler, input.inUV).xyz * input.inBaseColor.xyz;
    output.outAlbedo = float4(albedo, 1.0f);
    float3 N = normalize(input.inWorldNormal);
    output.outNormal = N.xy;
    output.outMaterial = float4(1.0f, 0.5f, input.inUV.x, float(N.z >= 0.0f));
    return output;
}
