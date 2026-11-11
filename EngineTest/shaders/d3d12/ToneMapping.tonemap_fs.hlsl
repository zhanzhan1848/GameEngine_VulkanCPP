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

static float2 _130;
static float4 _132;

struct SPIRV_Cross_Input
{
    float2 _130 : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 _132 : SV_Target0;
};

float3 _35(float3 _34)
{
    return clamp((_34 * ((_34 * 2.5099999904632568359375f) + 0.02999999932944774627685546875f.xxx)) / ((_34 * ((_34 * 2.4300000667572021484375f) + 0.589999973773956298828125f.xxx)) + 0.14000000059604644775390625f.xxx), 0.0f.xxx, 1.0f.xxx);
}

void frag_main()
{
    float3 _171 = 0.0f.xxx;
    bool _163 = false;
    bool _169 = false;
    float3 _160 = 0.0f.xxx;
    float _166 = 0.0f;
    if (false)
    {
        _132 = float4(abs(_28.Sample(_24, _130).xy) * 20.0f, 0.0f, 1.0f);
        return;
    }
    _160 = _21.Sample(_24, _130).xyz;
    float3 _190 = abs(_160);
    if (!any(bool3(_160.x != _160.x, _160.y != _160.y, _160.z != _160.z)))
    {
        _163 = any(bool3(_190.x > 3.3999999521443642490773241379936e+38f.xxx.x, _190.y > 3.3999999521443642490773241379936e+38f.xxx.y, _190.z > 3.3999999521443642490773241379936e+38f.xxx.z));
    }
    else
    {
        _163 = true;
    }
    if (_163)
    {
        _160 = 0.0f.xxx;
    }
    float4 _202 = _23.Sample(_24, _130);
    _166 = _29_m0._m0;
    if (_29_m0._m1 > 0.5f)
    {
        float _213 = exp2(_32.Load(int3(int2(0, 0), 0)).x);
        if (_213 > 9.9999999747524270787835121154785e-07f)
        {
            _169 = _213 < 1000000015047466219876688855040.0f;
        }
        else
        {
            _169 = false;
        }
        if (_169)
        {
            _166 = (_166 * _29_m0._m2) / _213;
        }
    }
    _171 = (_160 + (_202.xyz * _29_m0._m6)) * _166;
    _171 = _35(_171);
    _171 = pow(_171, 0.4545454680919647216796875f.xxx);
    if (_29_m0._m3 != 1.0f)
    {
        _171 = clamp(((_171 - 0.5f.xxx) * _29_m0._m3) + 0.5f.xxx, 0.0f.xxx, 1.0f.xxx);
    }
    if (_29_m0._m4 != 1.0f)
    {
        _171 = lerp(dot(_171, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)).xxx, _171, _29_m0._m4.xxx);
    }
    if (_29_m0._m5 > 0.0f)
    {
        _171 *= (1.0f - (_29_m0._m5 * smoothstep(0.3499999940395355224609375f, 0.85000002384185791015625f, distance(_130, 0.5f.xx))));
    }
    _132 = float4(_171, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    _130 = stage_input._130;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._132 = _132;
    return stage_output;
}
