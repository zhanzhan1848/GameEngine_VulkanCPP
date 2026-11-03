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

static float2 inUV;
static float4 inBaseColor;
static float3 inWorldNormal;
static float3 inWorldPos;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float4 inBaseColor : TEXCOORD3;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
};

void frag_main()
{
    float3 albedo = albedoTexture.Sample(albedoSampler, inUV).xyz * inBaseColor.xyz;
    float3 N = normalize(inWorldNormal);
    float3 L = normalize(lightData_dirAndIntensity.xyz);
    float3 V = normalize(viewData_cameraPos.xyz - inWorldPos);
    float3 H = normalize(L + V);
    float intensity = lightData_dirAndIntensity.w;
    float3 lightColor = lightData_color.xyz * intensity;
    float ndl = max(dot(N, L), 0.0f);
    float ndh = max(dot(N, H), 0.0f);
    float3 diffuse = albedo * ndl;
    float3 specular = (0.2199999988079071044921875f * pow(ndh, 48.0f)).xxx * ndl;
    float3 ambient = albedo * 0.1500000059604644775390625f;
    outColor = float4(ambient + (lightColor * (diffuse + specular)), 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUV = stage_input.inUV;
    inBaseColor = stage_input.inBaseColor;
    inWorldNormal = stage_input.inWorldNormal;
    inWorldPos = stage_input.inWorldPos;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outColor = outColor;
    return stage_output;
}
