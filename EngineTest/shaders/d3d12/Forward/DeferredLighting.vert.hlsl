static const float4 _20[3] = { float4(-1.0f, -1.0f, 0.0f, 1.0f), float4(-1.0f, 3.0f, 0.0f, 1.0f), float4(3.0f, -1.0f, 0.0f, 1.0f) };
static const float2 _29[3] = { 0.0f.xx, float2(0.0f, 2.0f), float2(2.0f, 0.0f) };

static float4 gl_Position;
static int gl_VertexIndex;
static float2 outUv;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 outUv : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    gl_Position = _20[gl_VertexIndex];
    outUv = _29[gl_VertexIndex];
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outUv = outUv;
    return stage_output;
}
