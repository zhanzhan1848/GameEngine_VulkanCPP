struct _7
{
    float4 _m0;
    float2 _m1;
};

struct _10
{
    float _m0;
    float _m1;
    float _m2;
    float _m3;
    float _m4;
    float _m5;
    float _m6;
    uint _m7;
};

static const float4 _74[3] = { float4(-1.0f, -1.0f, 0.0f, 1.0f), float4(3.0f, -1.0f, 0.0f, 1.0f), float4(-1.0f, 3.0f, 0.0f, 1.0f) };
static const float2 _79[3] = { float2(0.0f, 1.0f), float2(2.0f, 1.0f), float2(0.0f, -1.0f) };
static const _7 _82 = { 0.0f.xxxx, 0.0f.xx };

cbuffer _30_29 : register(b6, space0)
{
    _10 _29_m0 : packoffset(c0);
};

Texture2D<float4> _21 : register(t0, space0);
Texture2D<float4> _23 : register(t1, space0);
SamplerState _24 : register(s2, space0);
Texture2D<float4> _26 : register(t3, space0);
Texture2D<float4> _27 : register(t4, space0);
Texture2D<float4> _28 : register(t5, space0);
Texture2D<float4> _32 : register(t7, space0);

static float4 gl_Position;
static int gl_VertexIndex;
static float2 _65;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
};

struct SPIRV_Cross_Output
{
    float2 _65 : TEXCOORD0;
    float4 gl_Position : SV_Position;
};

void vert_main()
{
    _7 _80 = _82;
    _80._m0 = _74[uint(gl_VertexIndex)];
    _80._m1 = _79[uint(gl_VertexIndex)];
    gl_Position = _80._m0;
    gl_Position.y = -gl_Position.y;
    _65 = _80._m1;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output._65 = _65;
    return stage_output;
}
