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
static const _23 _355 = { 0.0f.xxxx, 0.0f.xx, 0.0f.xxx, 0.0f.xxx, 0.0f.xxx, 0u, 0u, 0u, 0u, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxx, 0.0f.xxx };
static const _25 _664 = { 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xx };

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
static float2 _606;
static float3 _609;
static float3 _612;
static float3 _614;
static uint _616;
static uint _618;
static uint _620;
static uint _622;
static float4 _624;
static float4 _626;
static float3 _628;
static float3 _630;
static float4 _635;
static float4 _636;
static float4 _637;
static float2 _638;

struct SPIRV_Cross_Input
{
    float2 _606 : TEXCOORD0;
    float3 _609 : TEXCOORD1;
    float3 _612 : TEXCOORD2;
    float3 _614 : TEXCOORD3;
    nointerpolation uint _616 : TEXCOORD4;
    nointerpolation uint _618 : TEXCOORD5;
    nointerpolation uint _620 : TEXCOORD6;
    nointerpolation uint _622 : TEXCOORD7;
    float4 _624 : TEXCOORD8;
    float4 _626 : TEXCOORD9;
    float3 _628 : TEXCOORD10;
    float3 _630 : TEXCOORD11;
    float4 gl_FragCoord : SV_Position;
    bool gl_FrontFacing : SV_IsFrontFace;
};

struct SPIRV_Cross_Output
{
    float4 _635 : SV_Target0;
    float4 _636 : SV_Target1;
    float4 _637 : SV_Target2;
    float2 _638 : SV_Target3;
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

float3 _250(uint _249)
{
    uint _257 = 0u;
    _257 = _249 * 2654435761u;
    _257 ^= (_257 >> 16u);
    _257 *= 2246822507u;
    _257 ^= (_257 >> 13u);
    _257 *= 3266489909u;
    _257 ^= (_257 >> 16u);
    return float3(float(_257 & 255u) / 255.0f, float((_257 >> 8u) & 255u) / 255.0f, float((_257 >> 16u) & 255u) / 255.0f);
}

void frag_main()
{
    float _680 = 0.0f;
    float3x3 _675 = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    bool _668 = false;
    _25 _662 = _664;
    bool _682 = false;
    float _677 = 1.0f;
    float3 _671 = 0.0f.xxx;
    float3 _665 = 0.0f.xxx;
    float _678 = 0.0f;
    bool _673 = false;
    float4 _667 = 1.0f.xxxx;
    _23 _602 = { gl_FragCoord, _606, _609, _612, _614, _616, _618, _620, _622, _624, _626, _628, _630 };
    if (_602._m5 == 4294967295u)
    {
        _662._m0 = float4(1.0f, 0.0f, 1.0f, 1.0f);
        _662._m1 = float4((_602._m2 * 0.5f) + 0.5f.xxx, 1.0f);
        _662._m2 = float4(1.0f, 0.5f, 0.0f, 1.0f);
        _662._m3 = 0.0f.xx;
        _635 = _662._m0;
        _636 = _662._m1;
        _637 = _662._m2;
        _638 = _662._m3;
        return;
    }
    _665 = normalize(_602._m2);
    if (dot(_665, normalize(cross(ddy(_602._m12), ddx(_602._m12)))) < 0.0f)
    {
        _665 = -_665;
    }
    float3 _718 = normalize(_602._m3);
    _22 _731 = _109(_602._m5);
    float2 _735 = frac(_602._m1 * _731._m7);
    uint _737_dummy_parameter;
    uint _739_dummy_parameter;
    uint _741_dummy_parameter;
    if (_731._m0 != 4294967295u)
    {
        _668 = _731._m0 < spvTextureSize(_57, 0u, _737_dummy_parameter).z;
    }
    else
    {
        _668 = false;
    }
    if (_668)
    {
        _667 = _57.SampleLevel(_61, float3(_735, float(_731._m0)), 0.0f);
    }
    else
    {
        _667 = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    _662._m0 = float4(_667.xyz * _731._m3, 1.0f);
    _671 = _665;
    if (_731._m1 != 4294967295u)
    {
        _673 = _731._m1 < spvTextureSize(_59, 0u, _739_dummy_parameter).z;
    }
    else
    {
        _673 = false;
    }
    if (_673)
    {
        float3 _782 = _59.SampleLevel(_61, float3(_735, float(_731._m1)), 0.0f).xyz;
        if (length(_782) > 0.100000001490116119384765625f)
        {
            _675[0u] = _718;
            _675[1u] = normalize(cross(_665, _718)) * ((dot(cross(normalize(_602._m2), _718), _602._m4) >= 0.0f) ? 1.0f : (-1.0f));
            _675[2u] = _665;
            _671 = normalize(mul((_782 * 2.0f) - 1.0f.xxx, _675));
        }
    }
    _662._m1 = float4((_671 * 0.5f) + 0.5f.xxx, 1.0f);
    _678 = max(_731._m5, 0.039999999105930328369140625f);
    _680 = _731._m4;
    if (_731._m2 != 4294967295u)
    {
        _682 = _731._m2 < spvTextureSize(_60, 0u, _741_dummy_parameter).z;
    }
    else
    {
        _682 = false;
    }
    if (_682)
    {
        float4 _818 = _60.SampleLevel(_61, float3(_735, float(_731._m2)), 0.0f);
        _677 = _818.x;
        _678 = _818.y;
        _680 = _818.z;
    }
    _662._m2 = float4(_677, _678, _680, float((_731._m8 >> 8u) & 255u) / 255.0f);
    if (_27_m0._m9 != 0u)
    {
        _662._m3 = (_602._m9.xy / _602._m9.w.xx) - (_602._m10.xy / _602._m10.w.xx);
    }
    else
    {
        _662._m3 = 0.0f.xx;
    }
    if (_27_m0._m10 != 0u)
    {
        if (_27_m0._m10 == 4u)
        {
            _662._m0 = float4((_665 * 0.5f) + 0.5f.xxx, 1.0f);
            _662._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
            _662._m2 = float4(1.0f, 1.0f, 0.0f, 1.0f);
        }
        else
        {
            if (_27_m0._m10 == 5u)
            {
                _662._m0 = float4((normalize(_602._m11) * 0.5f) + 0.5f.xxx, 1.0f);
                _662._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _662._m2 = float4(1.0f, 1.0f, 0.0f, 1.0f);
            }
            else
            {
                _662._m0 = float4(_250((_27_m0._m10 != 1u) ? ((_27_m0._m10 == 3u) ? _602._m8 : _602._m7) : _602._m6), 1.0f);
                _662._m1 = float4(0.5f, 0.5f, 1.0f, 1.0f);
                _662._m2 = float4(1.0f, 0.89999997615814208984375f, 0.0f, 1.0f);
            }
        }
    }
    _635 = _662._m0;
    _636 = _662._m1;
    _637 = _662._m2;
    _638 = _662._m3;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_FragCoord = stage_input.gl_FragCoord;
    gl_FragCoord.w = 1.0 / gl_FragCoord.w;
    gl_FrontFacing = stage_input.gl_FrontFacing;
    _606 = stage_input._606;
    _609 = stage_input._609;
    _612 = stage_input._612;
    _614 = stage_input._614;
    _616 = stage_input._616;
    _618 = stage_input._618;
    _620 = stage_input._620;
    _622 = stage_input._622;
    _624 = stage_input._624;
    _626 = stage_input._626;
    _628 = stage_input._628;
    _630 = stage_input._630;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output._635 = _635;
    stage_output._636 = _636;
    stage_output._637 = _637;
    stage_output._638 = _638;
    return stage_output;
}
