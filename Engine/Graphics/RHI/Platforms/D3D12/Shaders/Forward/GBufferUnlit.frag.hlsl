// Vulkan reference: Forward/GBufferUnlit.frag (main).
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
    float2 sceneData_padding : packoffset(c14);
    float4 sceneData_viewPos : packoffset(c15);
    row_major float4x4 sceneData_shadowMatrix0 : packoffset(c16);
    row_major float4x4 sceneData_shadowMatrix1 : packoffset(c20);
};

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
    output.outAlbedo = input.inInstanceBaseColor;
    output.outNormal = float4(0.5f, 0.5f, 1.0f, 1.0f);
    output.outORM = float4(1.0f, 1.0f, 0.0f, 0.0f);
    float2 currentNDC = input.inCurrentPos.xy / input.inCurrentPos.w.xx;
    float2 previousNDC = input.inPreviousPos.xy / input.inPreviousPos.w.xx;
    float2 currentNDC_NoJitter = currentNDC - sceneData_jitter;
    float2 previousNDC_NoJitter = previousNDC - sceneData_previousJitter;
    output.outVelocity = (currentNDC_NoJitter - previousNDC_NoJitter) * 0.5f;
    return output;
}
