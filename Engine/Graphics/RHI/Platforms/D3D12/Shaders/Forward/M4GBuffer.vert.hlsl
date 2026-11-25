// Vulkan reference: Forward/M4GBuffer.vert (main).
cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewData_viewProjection : packoffset(c0);
    float4 viewData_cameraPos : packoffset(c4);
};

cbuffer M1PushConsts
{
    row_major float4x4 pc_model : packoffset(c0);
    float4 pc_baseColor : packoffset(c4);
};

struct StageInput
{
    float3 in_position : TEXCOORD0;
    uint in_colorTSign : TEXCOORD1;
    uint2 in_normal : TEXCOORD2;
    uint2 in_tangent : TEXCOORD3;
    float2 in_uv : TEXCOORD4;
};

struct StageOutput
{
    float3 outWorldPos : TEXCOORD0;
    float3 outWorldNormal : TEXCOORD1;
    float2 outUV : TEXCOORD2;
    float4 outBaseColor : TEXCOORD3;
    float4 positionCS : SV_Position;
};

#include "../Common/MeshNormal.hlsli"

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4 worldPos = mul(float4(input.in_position, 1.0f), pc_model);
    output.outWorldPos = worldPos.xyz;
    float3x3 normalMatrix = float3x3(float3(pc_model[0].xyz), float3(pc_model[1].xyz), float3(pc_model[2].xyz));
    output.outWorldNormal = normalize(mul(UnpackNormal(input.in_normal), normalMatrix));
    output.outUV = float2(input.in_uv.x, 1.0f - input.in_uv.y);
    output.outBaseColor = pc_baseColor;
    output.positionCS = mul(worldPos, viewData_viewProjection);
    return output;
}
