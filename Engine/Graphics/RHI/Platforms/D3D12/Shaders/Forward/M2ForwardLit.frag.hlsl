// Vulkan reference: Forward/M2ForwardLit.frag (main).
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
    float ndlRaw = dot(N, L);
    float slope = clamp(sqrt(max(0.0f, 1.0f - (ndlRaw * ndlRaw))) / max(abs(ndlRaw), 0.25f), 0.0f, 4.0f);
    float3 offsetPos = input.inWorldPos + ((N * shadowData_shadowParams.x) * min(1.0f + slope, 4.0f));
    float bias = (shadowData_shadowParams.x * (0.5f + (2.0f * min(slope, 3.0f)))) / shadowData_shadowParams.y;
    float4 clip = mul(float4(offsetPos, 1.0f), shadowData_lightViewProjection);
    float3 sc = clip.xyz / max(clip.w, 1e-06f).xxx;
    float2 shadowUV = (sc.xy * 0.5f) + 0.5f.xx;
    float ref = sc.z - bias;
    float shadow = 1.0f;
    if (((((((shadowUV.x >= 0.0f) && (shadowUV.x <= 1.0f)) && (shadowUV.y >= 0.0f)) && (shadowUV.y <= 1.0f)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f)))
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
    float3 specular = (0.22f * pow(ndh, 48.0f)).xxx * ndl;
    float3 ambient = albedo * 0.15f;
    output.outColor = float4(ambient + ((lightColor * (diffuse + specular)) * shadow), 1.0f);
    return output;
}
