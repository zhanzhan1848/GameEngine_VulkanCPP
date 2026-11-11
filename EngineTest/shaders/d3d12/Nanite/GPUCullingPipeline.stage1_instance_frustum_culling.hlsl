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
    _23 _357 = _358;
    _23 _361 = _362;
    bool _355 = false;
    _23 _359 = _360;
    if (!(gl_GlobalInvocationID.x >= _37_m0._m13))
    {
        _355 = gl_GlobalInvocationID.x >= 100000u;
    }
    else
    {
        _355 = true;
    }
    if (_355)
    {
        return;
    }
    _8 _378;
    _378._m0 = asfloat(uint4x4(_31.Load4(gl_GlobalInvocationID.x * 192 + 0), _31.Load4(gl_GlobalInvocationID.x * 192 + 16), _31.Load4(gl_GlobalInvocationID.x * 192 + 32), _31.Load4(gl_GlobalInvocationID.x * 192 + 48)));
    _378._m1 = asfloat(uint4x4(_31.Load4(gl_GlobalInvocationID.x * 192 + 64), _31.Load4(gl_GlobalInvocationID.x * 192 + 80), _31.Load4(gl_GlobalInvocationID.x * 192 + 96), _31.Load4(gl_GlobalInvocationID.x * 192 + 112)));
    _378._m2 = _31.Load4(gl_GlobalInvocationID.x * 192 + 128);
    _378._m3 = _31.Load4(gl_GlobalInvocationID.x * 192 + 144);
    _378._m4 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 160));
    _378._m5 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 164));
    _378._m6 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 168));
    _378._m7 = _31.Load(gl_GlobalInvocationID.x * 192 + 172);
    _378._m8 = asfloat(_31.Load(gl_GlobalInvocationID.x * 192 + 176));
    _378._m9 = _31.Load(gl_GlobalInvocationID.x * 192 + 180);
    _378._m10 = _31.Load(gl_GlobalInvocationID.x * 192 + 184);
    _378._m11 = _31.Load(gl_GlobalInvocationID.x * 192 + 188);
    float4 _388 = mul(float4(float3(_378._m4, _378._m5, _378._m6), 1.0f), _37_m0._m0);
    if ((_388.z - _378._m8) > 0.0f)
    {
        _357._m0 = 0u;
        _357._m1 = gl_GlobalInvocationID.x;
        _357._m2 = 0u;
        _357._m3 = 0u;
        _167(gl_GlobalInvocationID.x, _357);
        return;
    }
    if (_388.z < ((-_37_m0._m5) - _378._m8))
    {
        _359._m0 = 0u;
        _359._m1 = gl_GlobalInvocationID.x;
        _359._m2 = 0u;
        _359._m3 = 0u;
        _167(gl_GlobalInvocationID.x, _359);
        return;
    }
    _361._m1 = gl_GlobalInvocationID.x;
    _361._m2 = 0u;
    if (false)
    {
        _361._m0 = 0u;
        _361._m3 = 0u;
    }
    else
    {
        _361._m0 = 1u;
        _361._m3 = uint(clamp(max(0.0f, -_388.z) * 100.0f, 0.0f, 4294967040.0f));
    }
    _167(gl_GlobalInvocationID.x, _361);
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
