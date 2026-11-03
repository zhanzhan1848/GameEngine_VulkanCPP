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

cbuffer ShadowData : register(b2, space0)
{
    row_major float4x4 shadowData_lightViewProjection : packoffset(c0);
    float4 shadowData_shadowParams : packoffset(c4);
};

Texture2D<float4> albedoTexture : register(t0, space1);
SamplerState albedoSampler : register(s1, space1);
Texture2D<float4> shadowMap : register(t2, space1);
SamplerComparisonState shadowSampler : register(s3, space1);

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
    float ndlRaw = dot(N, L);
    float slope = clamp(sqrt(max(0.0f, 1.0f - (ndlRaw * ndlRaw))) / max(abs(ndlRaw), 0.25f), 0.0f, 4.0f);
    float3 offsetPos = inWorldPos + ((N * shadowData_shadowParams.x) * min(1.0f + slope, 4.0f));
    float bias = (shadowData_shadowParams.x * (0.5f + (2.0f * min(slope, 3.0f)))) / shadowData_shadowParams.y;
    float4 clip = mul(float4(offsetPos, 1.0f), shadowData_lightViewProjection);
    float3 sc = clip.xyz / max(clip.w, 9.9999999747524270787835121154785e-07f).xxx;
    float2 shadowUV = (sc.xy * 0.5f) + 0.5f.xx;
    float ref = sc.z - bias;
    float shadow = 1.0f;
    bool _176 = shadowUV.x >= 0.0f;
    bool _182;
    if (_176)
    {
        _182 = shadowUV.x <= 1.0f;
    }
    else
    {
        _182 = _176;
    }
    bool _188;
    if (_182)
    {
        _188 = shadowUV.y >= 0.0f;
    }
    else
    {
        _188 = _182;
    }
    bool _194;
    if (_188)
    {
        _194 = shadowUV.y <= 1.0f;
    }
    else
    {
        _194 = _188;
    }
    bool _200;
    if (_194)
    {
        _200 = sc.z >= 0.0f;
    }
    else
    {
        _200 = _194;
    }
    bool _206;
    if (_200)
    {
        _206 = sc.z <= 1.0f;
    }
    else
    {
        _206 = _200;
    }
    if (_206)
    {
        float sum = 0.0f;
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                float2 tapUV = shadowUV + (float2(float(dx), float(dy)) * shadowData_shadowParams.z);
                float3 _251 = float3(tapUV, ref);
                sum += shadowMap.SampleCmp(shadowSampler, _251.xy, _251.z);
            }
        }
        shadow = sum / 9.0f;
    }
    float3 diffuse = albedo * ndl;
    float3 specular = (0.2199999988079071044921875f * pow(ndh, 48.0f)).xxx * ndl;
    float3 ambient = albedo * 0.1500000059604644775390625f;
    outColor = float4(ambient + ((lightColor * (diffuse + specular)) * shadow), 1.0f);
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
