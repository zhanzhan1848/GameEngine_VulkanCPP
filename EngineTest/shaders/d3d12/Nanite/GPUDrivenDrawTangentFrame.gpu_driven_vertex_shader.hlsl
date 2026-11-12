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

static float4 gl_Position;
static int gl_VertexIndex;
static int gl_InstanceIndex;
static float2 _315;
static float3 _317;
static float3 _319;
static float3 _320;
static uint _321;
static uint _323;
static uint _324;
static uint _325;
static float4 _326;
static float4 _327;
static float3 _328;
static float3 _329;

struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float2 _315 : TEXCOORD0;
    float3 _317 : TEXCOORD1;
    float3 _319 : TEXCOORD2;
    float3 _320 : TEXCOORD3;
    nointerpolation uint _321 : TEXCOORD4;
    nointerpolation uint _323 : TEXCOORD5;
    nointerpolation uint _324 : TEXCOORD6;
    nointerpolation uint _325 : TEXCOORD7;
    float4 _326 : TEXCOORD8;
    float4 _327 : TEXCOORD9;
    float3 _328 : TEXCOORD10;
    float3 _329 : TEXCOORD11;
    float4 gl_Position : SV_Position;
};

uint _292(uint _294, uint _295)
{
    return _294 / ((_295 == 0u) ? 1u : _295);
}

uint _300(uint _301, uint _302)
{
    return _301 % ((_302 == 0u) ? 1u : _302);
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

float3 _230(uint _228, uint _229)
{
    float2 _239 = (float2(float(_228 >> 16u), float(_228 & 65535u)) * 3.05180437862873077392578125e-05f) - 1.0f.xx;
    if (dot(_239, _239) > 1.0f)
    {
        return normalize(float3(_239, 0.0f));
    }
    return _184(_228, _229 >> 1u);
}

void vert_main()
{
    float3x3 _356 = float3x3(0.0f.xxx, 0.0f.xxx, 0.0f.xxx);
    float3 _361 = 0.0f.xxx;
    _23 _353 = _355;
    float3 _359 = 0.0f.xxx;
    _11 _368;
    _368._m0 = _45.Load4(_42.Load(uint(gl_InstanceIndex) * 4 + 0) * 16 + 0);
    _12 _377;
    _377._m0 = asfloat(uint4x4(_48.Load4(_368._m0.y * 192 + 0), _48.Load4(_368._m0.y * 192 + 16), _48.Load4(_368._m0.y * 192 + 32), _48.Load4(_368._m0.y * 192 + 48)));
    _377._m1 = asfloat(uint4x4(_48.Load4(_368._m0.y * 192 + 64), _48.Load4(_368._m0.y * 192 + 80), _48.Load4(_368._m0.y * 192 + 96), _48.Load4(_368._m0.y * 192 + 112)));
    _377._m2 = _48.Load4(_368._m0.y * 192 + 128);
    _377._m3 = _48.Load4(_368._m0.y * 192 + 144);
    _377._m4 = asfloat(_48.Load(_368._m0.y * 192 + 160));
    _377._m5 = asfloat(_48.Load(_368._m0.y * 192 + 164));
    _377._m6 = asfloat(_48.Load(_368._m0.y * 192 + 168));
    _377._m7 = _48.Load(_368._m0.y * 192 + 172);
    _377._m8 = asfloat(_48.Load(_368._m0.y * 192 + 176));
    _377._m9 = _48.Load(_368._m0.y * 192 + 180);
    _377._m10 = _48.Load(_368._m0.y * 192 + 184);
    _377._m11 = _48.Load(_368._m0.y * 192 + 188);
    _10 _380;
    _380._m0 = _30.Load4(_368._m0.x * 64 + 0);
    _380._m1 = asfloat(_30.Load4(_368._m0.x * 64 + 16));
    _380._m2 = asfloat(_30.Load4(_368._m0.x * 64 + 32));
    _380._m3 = asfloat(_30.Load4(_368._m0.x * 64 + 48));
    if (uint(gl_VertexIndex) >= (_380._m0.w * 3u))
    {
        _353._m0 = 0.0f.xxxx;
        _353._m1 = 0.0f.xx;
        _353._m2 = float3(0.0f, 0.0f, 1.0f);
        _353._m3 = float3(1.0f, 0.0f, 0.0f);
        _353._m4 = float3(0.0f, 1.0f, 0.0f);
        _353._m5 = _368._m0.z;
        _353._m6 = _368._m0.x;
        _353._m7 = 0u;
        _353._m8 = _368._m0.y;
        _353._m9 = _353._m0;
        _353._m10 = _353._m0;
        _353._m11 = float3(0.0f, 1.0f, 0.0f);
        gl_Position = _353._m0;
        gl_Position.y = -gl_Position.y;
        _315 = _353._m1;
        _317 = _353._m2;
        _319 = _353._m3;
        _320 = _353._m4;
        _321 = _353._m5;
        _323 = _353._m6;
        _324 = _353._m7;
        _325 = _353._m8;
        _326 = _353._m9;
        _327 = _353._m10;
        _328 = _353._m11;
        _329 = _353._m12;
        return;
    }
    uint _424 = _380._m0.y + uint(gl_VertexIndex);
    uint _434 = _380._m0.x + ((_36.Load(_292(_424, 4u) * 4 + 0) >> (_300(_424, 4u) * 8u)) & 255u);
    _20 _450 = _65(_33.Load(_434 * 4 + 0));
    float3 _453 = _184(_450._m1, _450._m0);
    float4 _469 = mul(float4(float3(asfloat(_39.Load((_33.Load(_434 * 4 + 0) * 3u) * 4 + 0)), asfloat(_39.Load(((_33.Load(_434 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_39.Load(((_33.Load(_434 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _377._m0);
    _353._m0 = mul(mul(_469, _27_m0._m0), _27_m0._m1);
    _353._m0 = float4(_353._m0.xy + (_27_m0._m13 * _353._m0.w), _353._m0.zw);
    _353._m9 = _353._m0;
    if (_27_m0._m9 != 0u)
    {
        float4 _508 = mul(mul(_469, _27_m0._m7), _27_m0._m8);
        _353._m10 = float4(_508.xy + (_27_m0._m14 * _508.w), _508.zw);
    }
    else
    {
        _353._m10 = _353._m9;
    }
    _353._m12 = _469.xyz;
    _356[0u] = _377._m0[0].xyz;
    _356[1u] = _377._m0[1].xyz;
    _356[2u] = _377._m0[2].xyz;
    _353._m11 = _453;
    _359 = normalize(mul(_453, _356));
    _361 = normalize(mul(_230(_450._m2, _450._m0), _356));
    if (abs(dot(_361, _359)) > 0.999000012874603271484375f)
    {
        bool3 _557 = (abs(_359.y) < 0.999000012874603271484375f).xxx;
        _361 = normalize(cross(float3(_557.x ? float3(1.0f, 0.0f, 0.0f).x : float3(0.0f, 1.0f, 0.0f).x, _557.y ? float3(1.0f, 0.0f, 0.0f).y : float3(0.0f, 1.0f, 0.0f).y, _557.z ? float3(1.0f, 0.0f, 0.0f).z : float3(0.0f, 1.0f, 0.0f).z), _359));
    }
    else
    {
        _361 = normalize(_361 - (_359 * dot(_361, _359)));
    }
    _353._m1 = float2(_450._m3.x, 1.0f - _450._m3.y);
    _353._m2 = _359;
    _353._m3 = _361;
    _353._m4 = cross(_359, _361) * (((_450._m0 & 16777216u) != 0u) ? (-1.0f) : 1.0f);
    _353._m5 = _368._m0.z;
    _353._m6 = _368._m0.x;
    _353._m7 = _292(uint(gl_VertexIndex), 3u);
    _353._m8 = _368._m0.y;
    gl_Position = _353._m0;
    gl_Position.y = -gl_Position.y;
    _315 = _353._m1;
    _317 = _353._m2;
    _319 = _353._m3;
    _320 = _353._m4;
    _321 = _353._m5;
    _323 = _353._m6;
    _324 = _353._m7;
    _325 = _353._m8;
    _326 = _353._m9;
    _327 = _353._m10;
    _328 = _353._m11;
    _329 = _353._m12;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    stage_output._315 = _315;
    stage_output._317 = _317;
    stage_output._319 = _319;
    stage_output._320 = _320;
    stage_output._321 = _321;
    stage_output._323 = _323;
    stage_output._324 = _324;
    stage_output._325 = _325;
    stage_output._326 = _326;
    stage_output._327 = _327;
    stage_output._328 = _328;
    stage_output._329 = _329;
    return stage_output;
}
