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

static float2 _105;
static float4 _108;

struct SPIRV_Cross_Input
{
    float2 _105 : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 _108 : SV_Target0;
};

void frag_main()
{
    _108 = float4((_21.Sample(_24, _105).xyz + _23.Sample(_24, _105).xyz) * _29_m0._m0, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    _105 = stage_input._105;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._108 = _108;
    return stage_output;
}
