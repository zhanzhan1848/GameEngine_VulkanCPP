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

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

void comp_main()
{
    float _799 = 0.100000001490116119384765625f;
    bool _794 = false;
    bool _788 = false;
    bool _800 = false;
    int _796 = 0;
    bool _790 = false;
    float _798 = 1.0f;
    bool _792 = false;
    uint _804;
    _57.InterlockedAdd(0, 0, _804);
    if (gl_GlobalInvocationID.x >= _804)
    {
        return;
    }
    uint _808 = gl_GlobalInvocationID.x * 12u;
    if (!(_43.Load((_808 + 0u) * 4 + 0) == 0u))
    {
        _788 = _37_m0._m7 == 0u;
    }
    else
    {
        _788 = true;
    }
    if (_788)
    {
        return;
    }
    float3 _835 = float3(asfloat(_43.Load((_808 + 5u) * 4 + 0)), asfloat(_43.Load((_808 + 6u) * 4 + 0)), asfloat(_43.Load((_808 + 7u) * 4 + 0)));
    float _839 = asfloat(_43.Load((_808 + 8u) * 4 + 0));
    float4 _843 = mul(float4(_835, 1.0f), _37_m0._m2);
    if (_843.w <= 0.0f)
    {
        return;
    }
    float2 _851 = _843.xy / _843.w.xx;
    float2 _858 = float2((_851.x * 0.5f) + 0.5f, 0.5f - (_851.y * 0.5f));
    if (!(_858.x < 0.0f))
    {
        _790 = _858.x > 1.0f;
    }
    else
    {
        _790 = true;
    }
    if (!_790)
    {
        _792 = _858.y < 0.0f;
    }
    else
    {
        _792 = true;
    }
    if (!_792)
    {
        _794 = _858.y > 1.0f;
    }
    else
    {
        _794 = true;
    }
    if (_794)
    {
        return;
    }
    if ((_839 / _843.w) < 0.07999999821186065673828125f)
    {
        return;
    }
    float _893 = length(_835 - _37_m0._m3.xyz);
    if (_893 < 5.0f)
    {
        _796 = 0;
    }
    else
    {
        if (_893 < 15.0f)
        {
            _796 = 1;
        }
        else
        {
            if (_893 < 30.0f)
            {
                _796 = 2;
            }
            else
            {
                _796 = 3;
            }
        }
    }
    uint _907_dummy_parameter;
    uint2 _907 = spvTextureSize(_55, uint(_796), _907_dummy_parameter);
    float4 _924 = _55.Load(int3(int2(uint2(uint(clamp(_858.x * float(_907.x), 0.0f, 4294967040.0f)), uint(clamp(_858.y * float(_907.y), 0.0f, 4294967040.0f)))), _796));
    float _925 = _924.x;
    float _928 = _843.z / _843.w;
    if (_893 < 10.0f)
    {
        _798 = 2.0f;
    }
    else
    {
        if (_893 < 25.0f)
        {
            _798 = 1.5f;
        }
    }
    if (_925 < (_928 - (((0.0199999995529651641845703125f * _798) * (1.0f + (float(_796) * 0.20000000298023223876953125f))) + (_839 * 0.1500000059604644775390625f))))
    {
        if (_893 < 8.0f)
        {
            _799 = 0.300000011920928955078125f;
        }
        else
        {
            if (_893 < 20.0f)
            {
                _799 = 0.20000000298023223876953125f;
            }
        }
        if ((_928 - _925) > _799)
        {
            if (_893 < 5.0f)
            {
                _800 = _796 > 0;
            }
            else
            {
                _800 = false;
            }
            if (_800)
            {
                return;
            }
            _43.Store((_808 + 0u) * 4 + 0, 0u);
            return;
        }
        else
        {
            return;
        }
    }
    else
    {
        return;
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
