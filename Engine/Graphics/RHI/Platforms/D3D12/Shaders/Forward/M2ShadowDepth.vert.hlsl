// Vulkan reference: Forward/M2ShadowDepth.vert (main).
cbuffer ShadowDepthData : register(b0, space0)
{
    row_major float4x4 shadowData_lightViewProjection : packoffset(c0);
};

cbuffer M2ShadowPush
{
    row_major float4x4 pc_model : packoffset(c0);
};

struct StageInput
{
    float3 in_position : TEXCOORD0;
};

struct StageOutput
{
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float4 worldPos = mul(float4(input.in_position, 1.0f), pc_model);
    output.positionCS = mul(worldPos, shadowData_lightViewProjection);
    output.positionCS.z += 0.001f;
    return output;
}
