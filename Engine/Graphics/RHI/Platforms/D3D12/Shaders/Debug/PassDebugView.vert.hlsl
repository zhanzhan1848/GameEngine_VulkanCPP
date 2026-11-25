// Vulkan reference: Debug/PassDebugView.vert (main).
static const float4 _20[3] = { float4(-1.0f, -1.0f, 0.0f, 1.0f), float4(-1.0f, 3.0f, 0.0f, 1.0f), float4(3.0f, -1.0f, 0.0f, 1.0f) };
static const float2 _29[3] = { 0.0f.xx, float2(0.0f, 2.0f), float2(2.0f, 0.0f) };

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float2 outUv : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    output.positionCS = _20[int(input.vertexID)];
    output.outUv = _29[int(input.vertexID)];
    return output;
}
