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
    uint4 _m0;
};

struct _13
{
    float4 _m0[6];
    row_major float4x4 _m1;
    uint4 _m2;
};

ByteAddressBuffer _22 : register(t0, space0);
ByteAddressBuffer _25 : register(t1, space0);
ByteAddressBuffer _28 : register(t2, space0);
cbuffer _32_31 : register(b3, space0)
{
    _13 _31_m0 : packoffset(c0);
};

RWByteAddressBuffer _34 : register(u4, space0);
RWByteAddressBuffer _37 : register(u5, space0);
RWByteAddressBuffer _40 : register(u6, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

bool _46(float3 _44, float _45)
{
    int _57 = 0;
    uint2 _71 = uint2(4294967295u, 4294967295u);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == _71.x, uint2(0u, 0u).y == _71.y)))
        {
            break;
        }
        _71 -= uint2(uint(_71.y == 0u), 1u);
        if (!(_57 < 6))
        {
            break;
        }
        if (dot(float4(_44, 1.0f), _31_m0._m0[_57]) < (-_45))
        {
            return false;
        }
        _57++;
        continue;
    }
    return true;
}

void comp_main()
{
    uint _120 = 0u;
    uint2 _152 = uint2(4294967295u, 4294967295u);
    if (gl_GlobalInvocationID.x >= _31_m0._m2.x)
    {
        return;
    }
    _8 _134;
    _134._m0 = asfloat(uint4x4(_22.Load4(gl_GlobalInvocationID.x * 192 + 0), _22.Load4(gl_GlobalInvocationID.x * 192 + 16), _22.Load4(gl_GlobalInvocationID.x * 192 + 32), _22.Load4(gl_GlobalInvocationID.x * 192 + 48)));
    _134._m1 = asfloat(uint4x4(_22.Load4(gl_GlobalInvocationID.x * 192 + 64), _22.Load4(gl_GlobalInvocationID.x * 192 + 80), _22.Load4(gl_GlobalInvocationID.x * 192 + 96), _22.Load4(gl_GlobalInvocationID.x * 192 + 112)));
    _134._m2 = _22.Load4(gl_GlobalInvocationID.x * 192 + 128);
    _134._m3 = _22.Load4(gl_GlobalInvocationID.x * 192 + 144);
    _134._m4 = asfloat(_22.Load(gl_GlobalInvocationID.x * 192 + 160));
    _134._m5 = asfloat(_22.Load(gl_GlobalInvocationID.x * 192 + 164));
    _134._m6 = asfloat(_22.Load(gl_GlobalInvocationID.x * 192 + 168));
    _134._m7 = _22.Load(gl_GlobalInvocationID.x * 192 + 172);
    _134._m8 = asfloat(_22.Load(gl_GlobalInvocationID.x * 192 + 176));
    _134._m9 = _22.Load(gl_GlobalInvocationID.x * 192 + 180);
    _134._m10 = _22.Load(gl_GlobalInvocationID.x * 192 + 184);
    _134._m11 = _22.Load(gl_GlobalInvocationID.x * 192 + 188);
    if (!_46(float3(_134._m4, _134._m5, _134._m6), _134._m8))
    {
        return;
    }
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == _152.x, uint2(0u, 0u).y == _152.y)))
        {
            break;
        }
        _152 -= uint2(uint(_152.y == 0u), 1u);
        if (!(_120 < _134._m2.w))
        {
            break;
        }
        uint _169 = _120;
        uint _170 = _134._m2.z + _169;
        _10 _173;
        _173._m0 = _28.Load4(_170 * 16 + 0);
        _9 _178;
        _178._m0 = _25.Load4(_173._m0.x * 64 + 0);
        _178._m1 = asfloat(_25.Load4(_173._m0.x * 64 + 16));
        _178._m2 = asfloat(_25.Load4(_173._m0.x * 64 + 32));
        _178._m3 = asfloat(_25.Load4(_173._m0.x * 64 + 48));
        if (!_46(mul(float4(float3(_178._m2.w, _178._m3.x, _178._m3.y), 1.0f), _134._m0).xyz, _178._m3.z))
        {
            uint _198 = _120;
            _120 = _198 + 1u;
            continue;
        }
        uint _196;
        _34.InterlockedAdd(0, 1u, _196);
        _37.Store(_196 * 4 + 0, _170);
        uint _198 = _120;
        _120 = _198 + 1u;
        continue;
    }
    if (gl_GlobalInvocationID.x == 0u)
    {
        _40.Store(0, 378u);
        _40.Store(4, 0u);
        _40.Store(8, 0u);
        _40.Store(12, 0u);
        return;
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
