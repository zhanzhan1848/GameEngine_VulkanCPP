// Vulkan reference: Forward/ForwardWater.frag (main).
cbuffer SceneData : register(b1, space0)
{
    row_major float4x4 sceneData_model : packoffset(c0);
    float4 sceneData_lightPos : packoffset(c4);
    float4 sceneData_lightColor : packoffset(c5);
    float4 sceneData_reflectionPlane : packoffset(c6);
    float4 sceneData_reflectionPlane2 : packoffset(c7);
    float4 sceneData_reflectionPlane3 : packoffset(c8);
    row_major float4x4 sceneData_previousModel : packoffset(c9);
    float2 sceneData_jitter : packoffset(c13);
    float2 sceneData_previousJitter : packoffset(c13.z);
    float sceneData_time : packoffset(c14);
    float sceneData_timePad : packoffset(c14.y);
    float4 sceneData_viewPos : packoffset(c15);
    row_major float4x4 sceneData_shadowMatrix0 : packoffset(c16);
    row_major float4x4 sceneData_shadowMatrix1 : packoffset(c20);
};

Texture2D<float4> albedoMap : register(t0, space1);
Texture2D<float4> normalMap : register(t1, space1);
Texture2D<float4> ormMap : register(t2, space1);
SamplerState defaultSampler : register(s3, space1);

struct StageInput
{
    float3 inWorldPos : TEXCOORD0;
    float3 inWorldNormal : TEXCOORD1;
    float2 inUV : TEXCOORD2;
    float4 inShadowPos0 : TEXCOORD3;
    float4 inShadowPos1 : TEXCOORD4;
    float4 inInstanceBaseColor : TEXCOORD5;
    float inInstanceRoughness : TEXCOORD6;
    float inInstanceMetallic : TEXCOORD7;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
};

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float denom = ((NdotH * NdotH) * (a2 - 1.0f)) + 1.0f;
    return a2 / (((3.1415927f * denom) * denom) + 0.0001f);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float t = sceneData_time;
    float3 waveNormal;
    waveNormal.x = (sin((input.inWorldPos.x * 2.0f) + (t * 1.5f)) * cos((input.inWorldPos.z * 1.8f) + (t * 1.2f))) * 0.15f;
    waveNormal.y = 1.0f;
    waveNormal.z = (cos((input.inWorldPos.x * 1.6f) + (t * 1.8f)) * sin((input.inWorldPos.z * 2.2f) + (t * 1.0f))) * 0.15f;
    float3 N = normalize(input.inWorldNormal + waveNormal);
    float3 viewDir = normalize(sceneData_viewPos.xyz - input.inWorldPos);
    float fresnel = pow(1.0f - max(dot(viewDir, N), 0.0f), 3.0f);
    float3 shallowColor = float3(0.1f, 0.6f, 0.7f);
    float3 deepColor = float3(0.02f, 0.1f, 0.3f);
    float3 waterColor = lerp(shallowColor, deepColor, fresnel.xxx) * input.inInstanceBaseColor.xyz;
    float opacity = lerp(0.4f, 0.95f, fresnel);
    float roughness = 0.05f;
    float metallic = 0.0f;
    float3 lightDir = normalize(sceneData_lightPos.xyz);
    float3 H = normalize(viewDir + lightDir);
    float3 F0 = lerp(0.04f.xxx, waterColor, metallic.xxx);
    float3 param = N;
    float3 param_1 = H;
    float param_2 = roughness;
    float NDF = DistributionGGX(param, param_1, param_2);
    float3 param_3 = N;
    float3 param_4 = viewDir;
    float3 param_5 = lightDir;
    float param_6 = roughness;
    float G = GeometrySmith(param_3, param_4, param_5, param_6);
    float param_7 = max(dot(H, viewDir), 0.0f);
    float3 param_8 = F0;
    float3 F = FresnelSchlick(param_7, param_8);
    float numerator = NDF * G;
    float denominator = ((4.0f * max(dot(N, viewDir), 0.0f)) * max(dot(N, lightDir), 0.0f)) + 0.0001f;
    float3 specular = (F * numerator) / denominator.xxx;
    float3 kS = F;
    float3 kD = (1.0f.xxx - kS) * (1.0f - metallic);
    float NdotL = max(dot(N, lightDir), 0.0f);
    float3 Lo = ((((kD * waterColor) / 3.1415927f.xxx) + specular) * sceneData_lightColor.xyz) * NdotL;
    float shadow = 1.0f;
    float3 sp0 = input.inShadowPos0.xyz / input.inShadowPos0.w.xxx;
    float3 _343 = sp0;
    float2 _348 = (_343.xy * 0.5f) + 0.5f.xx;
    sp0.x = _348.x;
    sp0.y = _348.y;
    sp0.y = 1.0f - sp0.y;
    if (((((((sp0.z > 0.0f) && (sp0.z < 1.0f)) && (sp0.x > 0.0f)) && (sp0.x < 1.0f)) && (sp0.y > 0.0f)) && (sp0.y < 1.0f)))
    {
        shadow = 0.5f + (0.5f * step(0.0f, sp0.z - 0.005f));
    }
    float3 ambient = 0.03f.xxx * waterColor;
    float3 color = ambient + (Lo * shadow);
    color /= (color + 1.0f.xxx);
    color = pow(color, 0.45454547f.xxx);
    output.outColor = float4(color, opacity);
    return output;
}
