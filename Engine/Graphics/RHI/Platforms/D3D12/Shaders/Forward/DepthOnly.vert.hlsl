// Vulkan reference: Forward/DepthOnly.vert (main).
struct InstanceData
{
    row_major float4x4 transform;
    float4 baseColor;
    float roughness;
    float metallic;
    float alphaCutoff;
    float _pad;
};

ByteAddressBuffer instanceData : register(t2, space0);
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
    uint instanceID : SV_InstanceID;
};

struct StageOutput
{
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4x4 _24;
    if (pc_use_pad.x != 0.0f)
    {
        float4x4 _38 = asfloat(uint4x4(instanceData.Load4(int(input.instanceID) * 96), instanceData.Load4(int(input.instanceID) * 96 + 16), instanceData.Load4(int(input.instanceID) * 96 + 32), instanceData.Load4(int(input.instanceID) * 96 + 48)));
        _24 = _38;
    }
    else
    {
        _24 = pc_transform;
    }
    float4x4 model = _24;
    float4 worldPos = mul(float4(input.in_position, 1.0f), model);
    output.positionCS = mul(worldPos, viewData_viewProjection);
    return output;
}
