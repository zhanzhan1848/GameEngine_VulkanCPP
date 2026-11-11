struct _8
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

struct _9
{
    uint4 _m0;
    float4 _m1;
    float4 _m2;
    float4 _m3;
};

struct _10
{
    uint _m0;
    uint _m1;
    uint _m2;
    uint _m3;
};

struct _11
{
    uint _m0;
    uint _m1;
    uint _m2;
    uint _m3;
};

struct _12
{
    row_major float4x4 _m0;
    row_major float4x4 _m1;
    row_major float4x4 _m2;
    float4 _m3;
    float _m4;
    float _m5;
    uint _m6;
    uint _m7;
    uint _m8;
    uint _m9;
    float _m10;
    float _m11;
    uint _m12;
    uint _m13;
    uint _m14;
    uint _m15;
    uint _m16;
    uint _m17;
    uint _m18;
    uint _m19;
    uint _m20;
};

struct _13
{
    uint _m0;
    uint _m1;
    uint _m2;
    uint _m3;
    uint _m4;
    float _m5;
    float _m6;
    float _m7;
    float _m8;
    uint _m9;
    uint _m10;
    uint _m11;
};

struct _23
{
    uint _m0;
    uint _m1;
    uint _m2;
    uint _m3;
};

static const float4 _102[6] = { 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx, 0.0f.xxxx };
static const _23 _142 = { 0u, 0u, 0u, 0u };
static const _23 _294 = { 0u, 0u, 0u, 0u };
static const _23 _358 = { 0u, 0u, 0u, 0u };
static const _23 _360 = { 0u, 0u, 0u, 0u };
static const _23 _362 = { 0u, 0u, 0u, 0u };
static const _23 _444 = { 0u, 0u, 0u, 0u };
static const _23 _492 = { 0u, 0u, 0u, 0u };
static const _23 _496 = { 0u, 0u, 0u, 0u };

ByteAddressBuffer _31 : register(t0, space0);
RWByteAddressBuffer _34 : register(u1, space0);
cbuffer _38_37 : register(b2, space0)
{
    _12 _37_m0 : packoffset(c0);
};

ByteAddressBuffer _40 : register(t3, space0);
RWByteAddressBuffer _43 : register(u4, space0);
RWByteAddressBuffer _46 : register(u5, space0);
RWByteAddressBuffer _49 : register(u6, space0);
RWByteAddressBuffer _52 : register(u7, space0);
RWByteAddressBuffer _57 : register(u9, space0);
RWByteAddressBuffer _60 : register(u10, space0);
ByteAddressBuffer _63 : register(t11, space0);
ByteAddressBuffer _66 : register(t12, space0);
Texture2D<float4> _55 : register(t8, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

_23 _136(uint _135)
{
    _23 _140 = _142;
    uint _144 = _135 * 4u;
    _140._m0 = _34.Load((_144 + 0u) * 4 + 0);
    _140._m1 = _34.Load((_144 + 1u) * 4 + 0);
    _140._m2 = _34.Load((_144 + 2u) * 4 + 0);
    _140._m3 = _34.Load((_144 + 3u) * 4 + 0);
    return _140;
}

void _167(uint _165, _23 _166)
{
    uint _171 = _165 * 4u;
    _34.Store((_171 + 0u) * 4 + 0, _166._m0);
    _34.Store((_171 + 1u) * 4 + 0, _166._m1);
    _34.Store((_171 + 2u) * 4 + 0, _166._m2);
    _34.Store((_171 + 3u) * 4 + 0, _166._m3);
}

void comp_main()
{
    bool _441 = false;
    _23 _443 = _444;
    if (!(gl_GlobalInvocationID.x >= _37_m0._m13))
    {
        _441 = gl_GlobalInvocationID.x >= 100000u;
    }
    else
    {
        _441 = true;
    }
    if (_441)
    {
        return;
    }
    _23 _458 = _136(gl_GlobalInvocationID.x);
    if (_458._m0 == 0u)
    {
        return;
    }
    _8 _464;
    _464._m0 = asfloat(uint4x4(_31.Load4(gl_GlobalInvocationID.x * 192 + 0), _31.Load4(gl_GlobalInvocationID.x * 192 + 16), _31.Load4(gl_GlobalInvocationID.x * 192 + 32), _31.Load4(gl_GlobalInvocationID.x * 192 + 48)));
    _464._m1 = asfloat(uint4x4(_31.Load4(gl_GlobalInvocationID.x * 192 + 64), _31.Load4(gl_GlobalInvocationID.x * 192 + 80), _31.Load4(gl_GlobalInvocationID.x * 192 + 96), _31.Load4(gl_GlobalInvocationID.x * 192 + 112)));
    _464._m2 = _31.Load4(gl_GlobalInvocationID.x * 192 + 128);
    _464._m3 = _31.Load4(gl_GlobalInvocationID.x * 192 + 144);
    _464._m4 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 160));
    _464._m5 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 164));
    _464._m6 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 168));
    _464._m7 = _31.Load(gl_GlobalInvocationID.x * 192 + 172);
    _464._m8 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 176));
    _464._m9 = _31.Load(gl_GlobalInvocationID.x * 192 + 180);
    _464._m10 = _31.Load(gl_GlobalInvocationID.x * 192 + 184);
    _464._m11 = _31.Load(gl_GlobalInvocationID.x * 192 + 188);
    _443 = _458;
    _443._m3 = uint(clamp(max(0.0f, -mul(float4(_464._m4, _464._m5, _464._m6, 1.0f), _37_m0._m0).z) * 10.0f, 0.0f, 4294967040.0f));
    _167(gl_GlobalInvocationID.x, _443);
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
