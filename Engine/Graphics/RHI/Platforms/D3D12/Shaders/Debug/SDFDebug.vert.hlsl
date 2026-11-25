// Vulkan reference: Debug/SDFDebug.vert (main).
static const float3 _20[6] = { float3(-1.0f, -1.0f, 0.0f), float3(1.0f, -1.0f, 0.0f), float3(-1.0f, 1.0f, 0.0f), float3(-1.0f, 1.0f, 0.0f), float3(1.0f, -1.0f, 0.0f), float3(1.0f, 1.0f, 0.0f) };

cbuffer DebugUniforms : register(b1, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 model : packoffset(c4);
    float slice_depth : packoffset(c8);
    float mode : packoffset(c8.y);
    float4 pad0 : packoffset(c9);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float2 vUV : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    float3 pos = _20[int(input.vertexID)];
    float4 worldPos = mul(float4(pos, 1.0f), model);
    output.positionCS = mul(worldPos, viewProjection);
    output.vUV = (pos.xy * 0.5f) + 0.5f.xx;
    return output;
}
