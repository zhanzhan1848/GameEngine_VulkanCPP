struct _8
{
    row_major float4x4 _m0;
    row_major float4x4 _m1;
    row_major float4x4 _m2;
    uint _m3;
    uint _m4;
    uint _m5;
    uint _m6;
    row_major float4x4 _m7;
    row_major float4x4 _m8;
    uint _m9;
    uint _m10;
    uint _m11;
    uint _m12;
    float2 _m13;
    float2 _m14;
};

struct _10
{
    uint4 _m0;
    float4 _m1;
    float4 _m2;
    float4 _m3;
};

struct _11
{
    uint4 _m0;
};

struct _12
{
    row_major float4x4 _m0;
    row_major float4x4 _m1;
    uint4 _m2;
    uint4 _m3;
    float _m4;
    float _m5;
    float _m6;
    uint _m7;
    float _m8;
    uint _m9;
    uint _m10;
    uint _m11;
};

struct _20
{
    uint _m0;
    uint _m1;
    uint _m2;
    float2 _m3;
};

struct _22
{
    uint _m0;
    uint _m1;
    uint _m2;
    float3 _m3;
    float _m4;
    float _m5;
    float _m6;
    float2 _m7;
    uint _m8;
};

struct _23
{
    float4 _m0;
    float2 _m1;
    float3 _m2;
    float3 _m3;
    float3 _m4;
    uint _m5;
    uint _m6;
    uint _m7;
    uint _m8;
    float4 _m9;
    float4 _m10;
    float3 _m11;
    float3 _m12;
};

struct _25
{
    float4 _m0;
    float4 _m1;
    float4 _m2;
    float2 _m3;
};

static const _20 _77 = { 0u, 0u, 0u, 0.0f.xx };
static const _22 _120 = { 0u, 0u, 0u, 0.0f.xxx, 0.0f, 0.0f, 0.0f, 0.0f.xx, 0u };
static const _23 _334 = { 0.0f.xxxx, 0.0f.xx, 0.0f.xxx, 0.0f.xxx, 0.0f.xxx, 0u, 0u, 0u, 0u, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxx, 0.0f.xxx };
static const _25 _643 = { 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xx };

cbuffer _28_27 : register(b0, space0)
{
    _8 _27_m0 : packoffset(c0);
};

ByteAddressBuffer _30 : register(t1, space0);
ByteAddressBuffer _33 : register(t2, space0);
ByteAddressBuffer _36 : register(t3, space0);
ByteAddressBuffer _39 : register(t4, space0);
ByteAddressBuffer _42 : register(t5, space0);
ByteAddressBuffer _45 : register(t6, space0);
ByteAddressBuffer _48 : register(t7, space0);
ByteAddressBuffer _51 : register(t8, space0);
ByteAddressBuffer _54 : register(t9, space0);
Texture2DArray<float4> _57 : register(t10, space0);
Texture2DArray<float4> _59 : register(t11, space0);
Texture2DArray<float4> _60 : register(t12, space0);
SamplerState _61 : register(s13, space0);

static float4 gl_FragCoord;
static bool gl_FrontFacing;
static float2 _585;
static float3 _588;
static float3 _591;
static float3 _593;
static uint _595;
static uint _597;
static uint _599;
static uint _601;
static float4 _603;
static float4 _605;
static float3 _607;
static float3 _609;
static float4 _614;
static float4 _615;
static float4 _616;
static float2 _617;

struct SPIRV_Cross_Input
{
    float2 _585 : TEXCOORD0;
    float3 _588 : TEXCOORD1;
    float3 _591 : TEXCOORD2;
    float3 _593 : TEXCOORD3;
    nointerpolation uint _595 : TEXCOORD4;
    nointerpolation uint _597 : TEXCOORD5;
    nointerpolation uint _599 : TEXCOORD6;
    nointerpolation uint _601 : TEXCOORD7;
    float4 _603 : TEXCOORD8;
    float4 _605 : TEXCOORD9;
    float3 _607 : TEXCOORD10;
    float3 _609 : TEXCOORD11;
    float4 gl_FragCoord : SV_Position;
    bool gl_FrontFacing : SV_IsFrontFace;
};

struct SPIRV_Cross_Output
{
    float4 _614 : SV_Target0;
    float4 _615 : SV_Target1;
    float4 _616 : SV_Target2;
    float2 _617 : SV_Target3;
};

uint3 spvTextureSize(Texture2DArray<float4> Tex, uint Level, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, ret.z, Param);
    return ret;
}

_22 _109(uint _108)
{
    _22 _118 = _120;
    uint _122 = _108 * 12u;
    _118._m0 = _54.Load((_122 + 0u) * 4 + 0);
    _118._m1 = _54.Load((_122 + 1u) * 4 + 0);
    _118._m2 = _54.Load((_122 + 2u) * 4 + 0);
    _118._m3 = float3(asfloat(_54.Load((_122 + 3u) * 4 + 0)), asfloat(_54.Load((_122 + 4u) * 4 + 0)), asfloat(_54.Load((_122 + 5u) * 4 + 0)));
    _118._m4 = asfloat(_54.Load((_122 + 6u) * 4 + 0));
    _118._m5 = asfloat(_54.Load((_122 + 7u) * 4 + 0));
    _118._m6 = asfloat(_54.Load((_122 + 8u) * 4 + 0));
    _118._m7 = float2(asfloat(_54.Load((_122 + 9u) * 4 + 0)), asfloat(_54.Load((_122 + 10u) * 4 + 0)));
    _118._m8 = _54.Load((_122 + 11u) * 4 + 0);
    return _118;
}

float3 _229(uint _228)
{
    uint _236 = 0u;
    _236 = _228 * 2654435761u;
    _236 ^= (_236 >> 16u);
    _236 *= 2246822507u;
    _236 ^= (_236 >> 13u);
    _236 *= 3266489909u;
    _236 ^= (_236 >> 16u);
    return float3(float(_236 & 255u) / 255.0f, float((_236 >> 8u) & 255u) / 255.0f, float((_236 >> 16u) & 255u) / 255.0f);
}

void frag_main()
{
    float _659 = 0.0f;
    float3x3 _654 = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    bool _647 = false;
    _25 _641 = _643;
    bool _661 = false;
    float _656 = 1.0f;
    float3 _650 = 0.0f.xxx;
    float3 _644 = 0.0f.xxx;
    float _657 = 0.0f;
    bool _652 = false;
    float4 _646 = 1.0f.xxxx;
    _23 _581 = { gl_FragCoord, _585, _588, _591, _593, _595, _597, _599, _601, _603, _605, _607, _609 };
    if (_581._m5 == 4294967295u)
    {
        _641._m0 = float4(1.0f, 0.0f, 1.0f, 1.0f);
        _641._m1 = float4((_581._m2 * 0.5f) + 0.5f.xxx, 1.0f);
        _641._m2 = float4(1.0f, 0.5f, 0.0f, 1.0f);
        _641._m3 = 0.0f.xx;
        _614 = _641._m0;
        _615 = _641._m1;
        _616 = _641._m2;
        _617 = _641._m3;
        return;
    }
    _644 = normalize(_581._m2);
    if (dot(_644, normalize(cross(ddy(_581._m12), ddx(_581._m12)))) < 0.0f)
    {
        _644 = -_644;
    }
    float3 _697 = normalize(_581._m3);
    _22 _702 = _109(_581._m5);
    float2 _705 = _581._m1 * _702._m7;
    float2 _706 = ddx(_705);
    float2 _707 = ddy(_705);
    float2 _708 = frac(_705);
    uint _710_dummy_parameter;
    uint _712_dummy_parameter;
    uint _714_dummy_parameter;
    if (_702._m0 != 4294967295u)
    {
        _647 = _702._m0 < spvTextureSize(_57, 0u, _710_dummy_parameter).z;
    }
    else
    {
        _647 = false;
    }
    if (_647)
    {
        _646 = _57.SampleGrad(_61, float3(_708, float(_702._m0)), _706, _707);
    }
    else
    {
        _646 = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    _641._m0 = float4(_646.xyz * _702._m3, 1.0f);
    _650 = _644;
    if (_702._m1 != 4294967295u)
    {
        _652 = _702._m1 < spvTextureSize(_59, 0u, _712_dummy_parameter).z;
    }
    else
    {
        _652 = false;
    }
    if (_652)
    {
        float3 _755 = _59.SampleLevel(_61, float3(_708, float(_702._m1)), 0.0f).xyz;
        if (length(_755) > 0.100000001490116119384765625f)
        {
            _654[0u] = _697;
            _654[1u] = normalize(cross(_644, _697));
            _654[2u] = _644;
            _650 = normalize(mul((_755 * 2.0f) - 1.0f.xxx, _654));
        }
    }
    _641._m1 = float4((_650 * 0.5f) + 0.5f.xxx, 1.0f);
    _657 = max(_702._m5, 0.039999999105930328369140625f);
    _659 = _702._m4;
    if (_702._m2 != 4294967295u)
    {
        _661 = _702._m2 < spvTextureSize(_60, 0u, _714_dummy_parameter).z;
    }
    else
    {
        _661 = false;
    }
    if (_661)
    {
        float4 _791 = _60.SampleLevel(_61, float3(_708, float(_702._m2)), 0.0f);
        _656 = _791.x;
        _657 = _791.y;
        _659 = _791.z;
    }
    _641._m2 = float4(_656, _657, _659, float((_702._m8 >> 8u) & 255u) / 255.0f);
    if (_27_m0._m9 != 0u)
    {
        _641._m3 = (_581._m9.xy / _581._m9.w.xx) - (_581._m10.xy / _581._m10.w.xx);
    }
    else
    {
        _641._m3 = 0.0f.xx;
    }
    if (_27_m0._m10 != 0u)
    {
        if (_27_m0._m10 == 4u)
        {
            _641._m0 = float4((_644 * 0.5f) + 0.5f.xxx, 1.0f);
            _641._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
            _641._m2 = float4(1.0f, 1.0f, 0.0f, 1.0f);
        }
        else
        {
            if (_27_m0._m10 == 5u)
            {
                _641._m0 = float4((normalize(_581._m11) * 0.5f) + 0.5f.xxx, 1.0f);
                _641._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _641._m2 = float4(1.0f, 1.0f, 0.0f, 1.0f);
            }
            else
            {
                _641._m0 = float4(_229((_27_m0._m10 != 1u) ? ((_27_m0._m10 == 3u) ? _581._m8 : _581._m7) : _581._m6), 1.0f);
                _641._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _641._m2 = float4(1.0f, 0.89999997615814208984375f, 0.0f, 1.0f);
            }
        }
    }
    _614 = _641._m0;
    _615 = _641._m1;
    _616 = _641._m2;
    _617 = _641._m3;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    gl_FrontFacing = stage_input.gl_FrontFacing;
    _585 = stage_input._585;
    _588 = stage_input._588;
    _591 = stage_input._591;
    _593 = stage_input._593;
    _595 = stage_input._595;
    _597 = stage_input._597;
    _599 = stage_input._599;
    _601 = stage_input._601;
    _603 = stage_input._603;
    _605 = stage_input._605;
    _607 = stage_input._607;
    _609 = stage_input._609;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._614 = _614;
    stage_output._615 = _615;
    stage_output._616 = _616;
    stage_output._617 = _617;
    return stage_output;
}
