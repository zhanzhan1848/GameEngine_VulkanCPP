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

static float4 gl_Position;
static int gl_VertexIndex;
static int gl_InstanceIndex;
static float2 _294;
static float3 _296;
static float3 _298;
static float3 _299;
static uint _300;
static uint _302;
static uint _303;
static uint _304;
static float4 _305;
static float4 _306;
static float3 _307;
static float3 _308;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float2 _294 : TEXCOORD0;
    float3 _296 : TEXCOORD1;
    float3 _298 : TEXCOORD2;
    float3 _299 : TEXCOORD3;
    nointerpolation uint _300 : TEXCOORD4;
    nointerpolation uint _302 : TEXCOORD5;
    nointerpolation uint _303 : TEXCOORD6;
    nointerpolation uint _304 : TEXCOORD7;
    float4 _305 : TEXCOORD8;
    float4 _306 : TEXCOORD9;
    float3 _307 : TEXCOORD10;
    float3 _308 : TEXCOORD11;
    float4 gl_Position : SV_Position;
};

uint _271(uint _273, uint _274)
{
    return _273 / ((_274 == 0u) ? 1u : _274);
}

uint _279(uint _280, uint _281)
{
    return _280 % ((_281 == 0u) ? 1u : _281);
}

_20 _65(uint _64)
{
    _20 _75 = _77;
    uint _79 = _64 * 6u;
    _75._m0 = _51.Load((_79 + 0u) * 4 + 0);
    _75._m1 = _51.Load((_79 + 1u) * 4 + 0);
    _75._m2 = _51.Load((_79 + 2u) * 4 + 0);
    _75._m3 = float2(asfloat(_51.Load((_79 + 4u) * 4 + 0)), asfloat(_51.Load((_79 + 5u) * 4 + 0)));
    return _75;
}

float3 _184(uint _182, uint _183)
{
    float2 _194 = 0.0f.xx;
    _194 = float2((float((_182 >> 16u) & 65535u) * 3.05180437862873077392578125e-05f) - 1.0f, (float(_182 & 65535u) * 3.05180437862873077392578125e-05f) - 1.0f);
    float _209 = dot(_194, _194);
    if (_209 > 1.0f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    return float3(_194.x, _194.y, sqrt(max(0.0f, 1.0f - _209)) * (float(((_183 >> 24u) & 255u) & 2u) - 1.0f));
}

void vert_main()
{
    float3x3 _335 = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    float3 _340 = 0.0f.xxx;
    _23 _332 = _334;
    float3 _338 = 0.0f.xxx;
    _11 _347;
    _347._m0 = _45.Load4(_42.Load(uint(gl_InstanceIndex) * 4 + 0) * 16 + 0);
    _12 _356;
    _356._m0 = asfloat(uint4x4(_48.Load4(_347._m0.y * 192 + 0), _48.Load4(_347._m0.y * 192 + 16), _48.Load4(_347._m0.y * 192 + 32), _48.Load4(_347._m0.y * 192 + 48)));
    _356._m1 = asfloat(uint4x4(_48.Load4(_347._m0.y * 192 + 64), _48.Load4(_347._m0.y * 192 + 80), _48.Load4(_347._m0.y * 192 + 96), _48.Load4(_347._m0.y * 192 + 112)));
    _356._m2 = _48.Load4(_347._m0.y * 192 + 128);
    _356._m3 = _48.Load4(_347._m0.y * 192 + 144);
    _356._m4 = asfloat(_48.Load(_347._m0.y * 192 + 160));
    _356._m5 = asfloat(_48.Load(_347._m0.y * 192 + 164));
    _356._m6 = asfloat(_48.Load(_347._m0.y * 192 + 168));
    _356._m7 = _48.Load(_347._m0.y * 192 + 172);
    _356._m8 = asfloat(_48.Load(_347._m0.y * 192 + 176));
    _356._m9 = _48.Load(_347._m0.y * 192 + 180);
    _356._m10 = _48.Load(_347._m0.y * 192 + 184);
    _356._m11 = _48.Load(_347._m0.y * 192 + 188);
    _10 _359;
    _359._m0 = _30.Load4(_347._m0.x * 64 + 0);
    _359._m1 = asfloat(_30.Load4(_347._m0.x * 64 + 16));
    _359._m2 = asfloat(_30.Load4(_347._m0.x * 64 + 32));
    _359._m3 = asfloat(_30.Load4(_347._m0.x * 64 + 48));
    if (uint(gl_VertexIndex) >= (_359._m0.w * 3u))
    {
        _332._m0 = 0.0f.xxxx;
        _332._m1 = 0.0f.xx;
        _332._m2 = float3(0.0f, 0.0f, 1.0f);
        _332._m3 = float3(1.0f, 0.0f, 0.0f);
        _332._m4 = float3(0.0f, 1.0f, 0.0f);
        _332._m5 = _347._m0.z;
        _332._m6 = _347._m0.x;
        _332._m7 = 0u;
        _332._m8 = _347._m0.y;
        _332._m9 = _332._m0;
        _332._m10 = _332._m0;
        _332._m11 = float3(0.0f, 1.0f, 0.0f);
        gl_Position = _332._m0;
        gl_Position.y = -gl_Position.y;
        _294 = _332._m1;
        _296 = _332._m2;
        _298 = _332._m3;
        _299 = _332._m4;
        _300 = _332._m5;
        _302 = _332._m6;
        _303 = _332._m7;
        _304 = _332._m8;
        _305 = _332._m9;
        _306 = _332._m10;
        _307 = _332._m11;
        _308 = _332._m12;
        return;
    }
    uint _403 = _359._m0.y + uint(gl_VertexIndex);
    uint _413 = _359._m0.x + ((_36.Load(_271(_403, 4u) * 4 + 0) >> (_279(_403, 4u) * 8u)) & 255u);
    _20 _429 = _65(_33.Load(_413 * 4 + 0));
    float3 _432 = _184(_429._m1, _429._m0);
    float4 _448 = mul(float4(float3(asfloat(_39.Load((_33.Load(_413 * 4 + 0) * 3u) * 4 + 0)), asfloat(_39.Load(((_33.Load(_413 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_39.Load(((_33.Load(_413 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _356._m0);
    _332._m0 = mul(mul(_448, _27_m0._m0), _27_m0._m1);
    _332._m0 = float4(_332._m0.xy + (_27_m0._m13 * _332._m0.w), _332._m0.zw);
    _332._m9 = _332._m0;
    if (_27_m0._m9 != 0u)
    {
        float4 _487 = mul(mul(_448, _27_m0._m7), _27_m0._m8);
        _332._m10 = float4(_487.xy + (_27_m0._m14 * _487.w), _487.zw);
    }
    else
    {
        _332._m10 = _332._m9;
    }
    _332._m12 = _448.xyz;
    _335[0u] = _356._m0[0].xyz;
    _335[1u] = _356._m0[1].xyz;
    _335[2u] = _356._m0[2].xyz;
    _332._m11 = _432;
    _338 = normalize(mul(_432, _335));
    _340 = normalize(mul(_184(_429._m2, _429._m0), _335));
    if (abs(dot(_340, _338)) > 0.999000012874603271484375f)
    {
        bool3 _536 = (abs(_338.y) < 0.999000012874603271484375f).xxx;
        _340 = normalize(cross(float3(_536.x ? float3(1.0f, 0.0f, 0.0f).x : float3(0.0f, 1.0f, 0.0f).x, _536.y ? float3(1.0f, 0.0f, 0.0f).y : float3(0.0f, 1.0f, 0.0f).y, _536.z ? float3(1.0f, 0.0f, 0.0f).z : float3(0.0f, 1.0f, 0.0f).z), _338));
    }
    else
    {
        _340 = normalize(_340 - (_338 * dot(_340, _338)));
    }
    _332._m1 = float2(_429._m3.x, 1.0f - _429._m3.y);
    _332._m2 = _338;
    _332._m3 = _340;
    _332._m4 = cross(_338, _340) * (((_429._m0 & 4278190080u) != 0u) ? (-1.0f) : 1.0f);
    _332._m5 = _347._m0.z;
    _332._m6 = _347._m0.x;
    _332._m7 = _271(uint(gl_VertexIndex), 3u);
    _332._m8 = _347._m0.y;
    gl_Position = _332._m0;
    gl_Position.y = -gl_Position.y;
    _294 = _332._m1;
    _296 = _332._m2;
    _298 = _332._m3;
    _299 = _332._m4;
    _300 = _332._m5;
    _302 = _332._m6;
    _303 = _332._m7;
    _304 = _332._m8;
    _305 = _332._m9;
    _306 = _332._m10;
    _307 = _332._m11;
    _308 = _332._m12;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output._294 = _294;
    stage_output._296 = _296;
    stage_output._298 = _298;
    stage_output._299 = _299;
    stage_output._300 = _300;
    stage_output._302 = _302;
    stage_output._303 = _303;
    stage_output._304 = _304;
    stage_output._305 = _305;
    stage_output._306 = _306;
    stage_output._307 = _307;
    stage_output._308 = _308;
    return stage_output;
}
