// Vulkan reference: Forward/M3Post.vert (main).
static const float2 _18[3] = { (-1.0f).xx, float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };
static const float2 _25[3] = { 0.0f.xx, float2(2.0f, 0.0f), float2(0.0f, 2.0f) };

struct StageInput
{
    uint vertexID : SV_VertexID;
};

struct StageOutput
{
    float2 outUV : TEXCOORD0;
    float4 positionCS : SV_Position;
};

StageOutput main(StageInput input)
{
    StageOutput output = (StageOutput)0;
    int vi = int(input.vertexID);
    output.outUV = _25[vi];
    output.positionCS = float4(_18[vi], 0.0f, 1.0f);
    return output;
}
