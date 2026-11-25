// Vulkan reference: Forward/Line.vert (main).
ByteAddressBuffer vb : register(t1, space0);
cbuffer PC
{
    row_major float4x4 pc_view_proj : packoffset(c0);
};

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.positionCS = mul(float4(asfloat(vb.Load3(int(input.vertexID) * 16)), 1.0f), pc_view_proj);
    return output;
}
