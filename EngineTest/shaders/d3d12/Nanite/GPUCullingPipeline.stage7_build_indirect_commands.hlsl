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

void comp_main()
{
    uint _1032 = 0u;
    if (gl_GlobalInvocationID.x != 0u)
    {
        return;
    }
    uint _1039;
    _46.InterlockedAdd(0, 0, _1039);
    _1032 = _1039;
    if (_1032 > _37_m0._m14)
    {
        _1032 = _37_m0._m14;
    }
    if (_1032 > 100000u)
    {
        _1032 = 100000u;
    }
    _52.Store(0, 384u);
    _52.Store(4, _1032);
    _52.Store(8, 0u);
    _52.Store(12, 0u);
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
