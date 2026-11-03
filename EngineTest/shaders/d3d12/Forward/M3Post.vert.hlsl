static const float2 _18[3] = { (-1.0f).xx, float2(3.0f, -1.0f), float2(-1.0f, 3.0f) };
static const float2 _25[3] = { 0.0f.xx, float2(2.0f, 0.0f), float2(0.0f, 2.0f) };

static float4 gl_Position;
static int gl_VertexIndex;
static float2 outUV;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 outUV : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    int vi = gl_VertexIndex;
    outUV = _25[vi];
    gl_Position = float4(_18[vi], 0.0f, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output.outUV = outUV;
    return stage_output;
}
