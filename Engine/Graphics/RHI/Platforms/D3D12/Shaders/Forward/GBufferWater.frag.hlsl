// Vulkan reference: Forward/GBufferWater.frag (main).
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
    float3 inWorldTangent : TEXCOORD2;
    float2 inUV : TEXCOORD3;
    float4 inCurrentPos : TEXCOORD4;
    float4 inPreviousPos : TEXCOORD5;
    float4 inInstanceBaseColor : TEXCOORD6;
    float inInstanceRoughness : TEXCOORD7;
    float inInstanceMetallic : TEXCOORD8;
};

struct StageOutput
{
    float4 outAlbedo : SV_Target0;
    float4 outNormal : SV_Target1;
    float4 outORM : SV_Target2;
    float2 outVelocity : SV_Target3;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float t = sceneData_time;
    float3 waveNormal;
    waveNormal.x = (sin((input.inWorldPos.x * 2.0f) + (t * 1.5f)) * cos((input.inWorldPos.z * 1.8f) + (t * 1.2f))) * 0.15f;
    waveNormal.y = 1.0f;
    waveNormal.z = (cos((input.inWorldPos.x * 1.6f) + (t * 1.8f)) * sin((input.inWorldPos.z * 2.2f) + (t * 1.0f))) * 0.15f;
    waveNormal = normalize(waveNormal);
    float3 N = normalize(input.inWorldNormal + waveNormal);
    float3 viewDir = normalize(sceneData_viewPos.xyz - input.inWorldPos);
    float fresnel = pow(1.0f - max(dot(viewDir, N), 0.0f), 3.0f);
    float3 shallowColor = float3(0.1f, 0.6f, 0.7f);
    float3 deepColor = float3(0.02f, 0.1f, 0.3f);
    float3 waterColor = lerp(shallowColor, deepColor, fresnel.xxx);
    waterColor *= input.inInstanceBaseColor.xyz;
    float opacity = lerp(0.4f, 0.95f, fresnel);
    output.outAlbedo = float4(waterColor, opacity);
    output.outNormal = float4((N * 0.5f) + 0.5f.xxx, 1.0f);
    output.outORM = float4(1.0f, 0.05f, 0.0f, 0.0f);
    float2 currentNDC = input.inCurrentPos.xy / input.inCurrentPos.w.xx;
    float2 previousNDC = input.inPreviousPos.xy / input.inPreviousPos.w.xx;
    float2 currentNDC_NoJitter = currentNDC - sceneData_jitter;
    float2 previousNDC_NoJitter = previousNDC - sceneData_previousJitter;
    output.outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5f;
    return output;
}
