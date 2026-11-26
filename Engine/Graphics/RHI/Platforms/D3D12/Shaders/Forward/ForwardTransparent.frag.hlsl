// Vulkan reference: Forward/ForwardTransparent.frag (main).
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
SamplerState defaultSampler : register(s3, space1);
Texture2D<float4> ormMap : register(t2, space1);
Texture2D<float4> normalMap : register(t1, space1);

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
    float4 albedoSample = albedoMap.Sample(defaultSampler, input.inUV);
    float3 albedo = albedoSample.xyz * input.inInstanceBaseColor.xyz;
    float alpha = albedoSample.w * input.inInstanceBaseColor.w;
    float4 ormSample = ormMap.Sample(defaultSampler, input.inUV);
    float roughness = input.inInstanceRoughness;
    float metallic = input.inInstanceMetallic;
    if (length(ormSample.xyz) > 0.01f)
    {
        roughness = ormSample.y * input.inInstanceRoughness;
        metallic = ormSample.z * input.inInstanceMetallic;
    }
    roughness = max(roughness, 0.04f);
    float3 N = normalize(input.inWorldNormal);
    float3 normalSample = normalMap.Sample(defaultSampler, input.inUV).xyz;
    if (length(normalSample) > 0.1f)
    {
        float3 up = float3(0.0f, 1.0f, 0.0f);
        if (length(cross(N, up)) < 0.01f)
        {
            up = float3(1.0f, 0.0f, 0.0f);
        }
        float3 tangent = normalize(cross(N, up));
        float3 bitangent = cross(N, tangent);
        float3x3 TBN = float3x3(float3(tangent), float3(bitangent), float3(N));
        N = normalize(mul((normalSample * 2.0f) - 1.0f.xxx, TBN));
    }
    float3 lightDir = normalize(sceneData_lightPos.xyz);
    float3 viewDir = normalize(sceneData_viewPos.xyz - input.inWorldPos);
    float3 H = normalize(viewDir + lightDir);
    float3 F0 = lerp(0.04f.xxx, albedo, metallic.xxx);
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
    float3 Lo = ((((kD * albedo) / 3.1415927f.xxx) + specular) * sceneData_lightColor.xyz) * NdotL;
    float shadow = 1.0f;
    float3 sp0 = input.inShadowPos0.xyz / input.inShadowPos0.w.xxx;
    float3 _377 = sp0;
    float2 _382 = (_377.xy * 0.5f) + 0.5f.xx;
    sp0.x = _382.x;
    sp0.y = _382.y;
    sp0.y = 1.0f - sp0.y;
    if (((((((sp0.z > 0.0f) && (sp0.z < 1.0f)) && (sp0.x > 0.0f)) && (sp0.x < 1.0f)) && (sp0.y > 0.0f)) && (sp0.y < 1.0f)))
    {
        shadow = 0.5f + (0.5f * step(0.0f, sp0.z - 0.005f));
    }
    float3 ambient = 0.03f.xxx * albedo;
    float3 color = ambient + (Lo * shadow);
    color /= (color + 1.0f.xxx);
    color = pow(color, 0.45454547f.xxx);
    output.outColor = float4(color, alpha);
    return output;
}
