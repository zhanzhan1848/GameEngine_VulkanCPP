// Vulkan reference: Forward/GBufferFoliage.vert (main).
struct InstanceData
{
    row_major float4x4 transform;
    float4 baseColor;
    float roughness;
    float metallic;
    float alphaCutoff;
    float transmission;
};

ByteAddressBuffer instanceData : register(t2, space0);
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
    row_major float4x4 viewData_invViewProjection : packoffset(c4);
    row_major float4x4 viewData_previousViewProjection : packoffset(c8);
};

cbuffer PushConsts
{
    row_major float4x4 pc_transform : packoffset(c0);
    float4 pc_use_pad : packoffset(c4);
};

struct StageInput
{
    float3 in_position : TEXCOORD0;
    uint in_colorTSign : TEXCOORD1;
    uint2 in_normal : TEXCOORD2;
    uint2 in_tangent : TEXCOORD3;
    float2 in_uv : TEXCOORD4;
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float3 outWorldPos : TEXCOORD0;
    float3 outWorldNormal : TEXCOORD1;
    float3 outWorldTangent : TEXCOORD2;
    float2 outUV : TEXCOORD3;
    float4 outCurrentPos : TEXCOORD4;
    float4 outPreviousPos : TEXCOORD5;
    float4 outInstanceBaseColor : TEXCOORD6;
    float outInstanceRoughness : TEXCOORD7;
    float outInstanceMetallic : TEXCOORD8;
    float outInstanceTransmission : TEXCOORD9;
    float4 positionCS : SV_Position;
};

#include "../Common/MeshNormal.hlsli"

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4x4 model;
    if (pc_use_pad.x != 0.0f)
    {
        InstanceData _81;
        _81.transform = asfloat(uint4x4(instanceData.Load4(int(input.instanceID) * 96), instanceData.Load4(int(input.instanceID) * 96 + 16), instanceData.Load4(int(input.instanceID) * 96 + 32), instanceData.Load4(int(input.instanceID) * 96 + 48)));
        _81.baseColor = asfloat(instanceData.Load4(int(input.instanceID) * 96 + 64));
        _81.roughness = asfloat(instanceData.Load(int(input.instanceID) * 96 + 80));
        _81.metallic = asfloat(instanceData.Load(int(input.instanceID) * 96 + 84));
        _81.alphaCutoff = asfloat(instanceData.Load(int(input.instanceID) * 96 + 88));
        _81.transmission = asfloat(instanceData.Load(int(input.instanceID) * 96 + 92));
        InstanceData inst;
        inst.transform = _81.transform;
        inst.baseColor = _81.baseColor;
        inst.roughness = _81.roughness;
        inst.metallic = _81.metallic;
        inst.alphaCutoff = _81.alphaCutoff;
        inst.transmission = _81.transmission;
        model = inst.transform;
        output.outInstanceBaseColor = inst.baseColor;
        output.outInstanceRoughness = inst.roughness;
        output.outInstanceMetallic = inst.metallic;
        output.outInstanceTransmission = inst.transmission;
    }
    else
    {
        model = pc_transform;
        output.outInstanceBaseColor = 1.0f.xxxx;
        output.outInstanceRoughness = 0.5f;
        output.outInstanceMetallic = 0.0f;
        output.outInstanceTransmission = 0.0f;
    }
    float4 worldPos = mul(float4(input.in_position, 1.0f), model);
    float heightFactor = clamp(input.in_position.y / 2.0f, 0.0f, 1.0f);
    float windX = (sin(((sceneData_padding.x * 1.5f) + (worldPos.x * 0.8f)) + (worldPos.z * 0.3f)) * 0.15f) * heightFactor;
    float windZ = (sin(((sceneData_padding.x * 1.2f) + (worldPos.z * 0.6f)) + (worldPos.x * 0.4f)) * 0.15f) * heightFactor;
    worldPos.x += windX;
    worldPos.z += windZ;
    output.outWorldPos = worldPos.xyz;
    float3x3 normalMatrix = float3x3(float3(model[0].xyz), float3(model[1].xyz), float3(model[2].xyz));
    float3 rawNormal = UnpackNormal(input.in_normal);
    output.outWorldNormal = normalize(mul(rawNormal, normalMatrix));
    float3 rawTangent = UnpackNormal(input.in_tangent);
    output.outWorldTangent = normalize(mul(rawTangent, normalMatrix));
    output.outUV = float2(input.in_uv.x, 1.0f - input.in_uv.y);
    output.positionCS = mul(worldPos, viewData_viewProjection);
    output.outCurrentPos = output.positionCS;
    output.outPreviousPos = mul(mul(float4(input.in_position, 1.0f), sceneData_previousModel), viewData_previousViewProjection);
    return output;
}
