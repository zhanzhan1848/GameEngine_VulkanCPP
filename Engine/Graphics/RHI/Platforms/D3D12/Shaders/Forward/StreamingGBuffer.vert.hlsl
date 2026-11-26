// Vulkan reference: Forward/StreamingGBuffer.vert (main).
ByteAddressBuffer idxBuf : register(t5, space0);
ByteAddressBuffer posBuf : register(t3, space0);
ByteAddressBuffer elemBuf : register(t4, space0);
cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    row_major float4x4 viewData_invViewProjection : packoffset(c4);
    row_major float4x4 viewData_previousViewProjection : packoffset(c8);
};

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

cbuffer PushConsts
{
    row_major float4x4 pc_transform : packoffset(c0);
    float4 pc_use_pad : packoffset(c4);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float3 outWorldPos : TEXCOORD0;
    float3 outWorldNormal : TEXCOORD1;
    float3 outWorldTangent : TEXCOORD2;
    float2 outUV : TEXCOORD3;
    float4 outCurrentPos : TEXCOORD4;
    float4 outPreviousPos : TEXCOORD5;
    float4 positionCS : SV_Position;
};

#include "../Common/MeshNormal.hlsli"

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    uint vid = idxBuf.Load(int(input.vertexID) * 4);
    float4x4 model = pc_transform;
    float3 rawPos = float3(asfloat(posBuf.Load(((vid * 3u) + 0u) * 4)), asfloat(posBuf.Load(((vid * 3u) + 1u) * 4)), asfloat(posBuf.Load(((vid * 3u) + 2u) * 4)));
    uint elemBase = vid * 5u;
    uint normalWord = elemBuf.Load((elemBase + 1u) * 4);
    uint2 rawNormalPacked = uint2(normalWord & 65535u, normalWord >> 16u);
    uint tangentWord = elemBuf.Load((elemBase + 2u) * 4);
    uint2 rawTangentPacked = uint2(tangentWord & 65535u, tangentWord >> 16u);
    uint uvWord0 = elemBuf.Load((elemBase + 3u) * 4);
    uint uvWord1 = elemBuf.Load((elemBase + 4u) * 4);
    float2 rawUV = float2(asfloat(uvWord0), asfloat(uvWord1));
    float4 worldPos = mul(float4(rawPos, 1.0f), model);
    output.outWorldPos = worldPos.xyz;
    float3x3 normalMatrix = float3x3(float3(model[0].xyz), float3(model[1].xyz), float3(model[2].xyz));
    float3 rawNormal = UnpackNormal(rawNormalPacked);
    output.outWorldNormal = normalize(mul(rawNormal, normalMatrix));
    float3 rawTangent = UnpackNormal(rawTangentPacked);
    output.outWorldTangent = normalize(mul(rawTangent, normalMatrix));
    output.outUV = float2(rawUV.x, 1.0f - rawUV.y);
    output.positionCS = mul(worldPos, viewData_viewProjection);
    output.outCurrentPos = output.positionCS;
    output.outPreviousPos = mul(mul(float4(rawPos, 1.0f), sceneData_previousModel), viewData_previousViewProjection);
    return output;
}
