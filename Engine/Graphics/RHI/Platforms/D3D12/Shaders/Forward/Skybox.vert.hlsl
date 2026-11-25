// Vulkan reference: Forward/Skybox.vert (main).
static const float3 _23[36] = { (-1.0f).xxx, float3(-1.0f, 1.0f, -1.0f), float3(1.0f, 1.0f, -1.0f), float3(1.0f, 1.0f, -1.0f), float3(1.0f, -1.0f, -1.0f), (-1.0f).xxx, float3(-1.0f, -1.0f, 1.0f), float3(1.0f, -1.0f, 1.0f), 1.0f.xxx, 1.0f.xxx, float3(-1.0f, 1.0f, 1.0f), float3(-1.0f, -1.0f, 1.0f), float3(-1.0f, 1.0f, 1.0f), float3(-1.0f, 1.0f, -1.0f), (-1.0f).xxx, (-1.0f).xxx, float3(-1.0f, -1.0f, 1.0f), float3(-1.0f, 1.0f, 1.0f), 1.0f.xxx, float3(1.0f, -1.0f, 1.0f), float3(1.0f, -1.0f, -1.0f), float3(1.0f, -1.0f, -1.0f), float3(1.0f, 1.0f, -1.0f), 1.0f.xxx, (-1.0f).xxx, float3(1.0f, -1.0f, -1.0f), float3(1.0f, -1.0f, 1.0f), float3(1.0f, -1.0f, 1.0f), float3(-1.0f, -1.0f, 1.0f), (-1.0f).xxx, float3(-1.0f, 1.0f, -1.0f), float3(-1.0f, 1.0f, 1.0f), 1.0f.xxx, 1.0f.xxx, float3(1.0f, 1.0f, -1.0f), float3(-1.0f, 1.0f, -1.0f) };

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

cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    row_major float4x4 viewData_previousViewProjection : packoffset(c4);
    row_major float4x4 viewData_invViewProjection : packoffset(c8);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float3 outUv : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 pos = _23[int(input.vertexID)];
    output.outUv = pos;
    float3 camPos = sceneData_viewPos.xyz;
    float4 worldPos = float4(pos + camPos, 1.0f);
    output.positionCS = mul(worldPos, viewData_viewProjection);
    output.positionCS.z = output.positionCS.w;
    return output;
}
