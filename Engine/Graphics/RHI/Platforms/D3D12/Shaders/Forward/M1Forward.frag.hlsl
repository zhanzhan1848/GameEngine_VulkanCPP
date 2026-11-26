// Vulkan reference: Forward/M1Forward.frag (main).
cbuffer LightData : register(b1, space0)
{
    float4 lightData_dirAndIntensity : packoffset(c0);
    float4 lightData_color : packoffset(c1);
};

cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    float4 viewData_cameraPos : packoffset(c4);
};

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
    float4 outColor : SV_Target0;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 albedo = albedoTexture.Sample(albedoSampler, input.inUV).xyz * input.inBaseColor.xyz;
    float3 N = normalize(input.inWorldNormal);
    float3 L = normalize(lightData_dirAndIntensity.xyz);
    float3 V = normalize(viewData_cameraPos.xyz - input.inWorldPos);
    float3 H = normalize(L + V);
    float intensity = lightData_dirAndIntensity.w;
    float3 lightColor = lightData_color.xyz * intensity;
    float ndl = max(dot(N, L), 0.0f);
    float ndh = max(dot(N, H), 0.0f);
    float3 diffuse = albedo * ndl;
    float3 specular = (0.22f * pow(ndh, 48.0f)).xxx * ndl;
    float3 ambient = albedo * 0.15f;
    output.outColor = float4(ambient + (lightColor * (diffuse + specular)), 1.0f);
    return output;
}
