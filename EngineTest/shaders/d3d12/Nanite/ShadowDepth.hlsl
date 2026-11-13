struct _9
{
    row_major float4x4 _m0;
    row_major float4x4 _m1;
    uint4 _m2;
    uint4 _m3;
    float3 _m4;
    float _m5;
    uint4 _m6;
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
    uint4 _m1;
};

struct _18
{
    float4 _m0;
};

static const _18 _97 = { 0.0f.xxxx };

cbuffer _20_19 : register(b0, space0)
{
    _12 _19_m0 : packoffset(c0);
};

ByteAddressBuffer _22 : register(t1, space0);
ByteAddressBuffer _25 : register(t2, space0);
ByteAddressBuffer _28 : register(t3, space0);
ByteAddressBuffer _31 : register(t4, space0);
ByteAddressBuffer _34 : register(t5, space0);
ByteAddressBuffer _37 : register(t6, space0);
ByteAddressBuffer _40 : register(t7, space0);

static float4 gl_Position;
static int gl_VertexIndex;
static int gl_InstanceIndex;
struct SPIRV_Cross_Input
{
    uint gl_VertexIndex : SV_VertexID;
    uint gl_InstanceIndex : SV_InstanceID;
};

struct SPIRV_Cross_Output
{
    float4 gl_Position : SV_Position;
};

uint _43(uint _45, uint _46)
{
    return _45 / ((_46 == 0u) ? 1u : _46);
}

uint _54(uint _55, uint _56)
{
    return _55 % ((_56 == 0u) ? 1u : _56);
}

void vert_main()
{
    _18 _95 = _97;
    float4 _98 = 0.0f.xxxx;
    uint _102 = _43(uint(gl_VertexIndex), 3u);
    _11 _109;
    _109._m0 = _25.Load4(_22.Load(uint(gl_InstanceIndex) * 4 + 0) * 16 + 0);
    _9 _116;
    _116._m0 = asfloat(uint4x4(_28.Load4(_109._m0.y * 192 + 0), _28.Load4(_109._m0.y * 192 + 16), _28.Load4(_109._m0.y * 192 + 32), _28.Load4(_109._m0.y * 192 + 48)));
    _116._m1 = asfloat(uint4x4(_28.Load4(_109._m0.y * 192 + 64), _28.Load4(_109._m0.y * 192 + 80), _28.Load4(_109._m0.y * 192 + 96), _28.Load4(_109._m0.y * 192 + 112)));
    _116._m2 = _28.Load4(_109._m0.y * 192 + 128);
    _116._m3 = _28.Load4(_109._m0.y * 192 + 144);
    _116._m4 = asfloat(_28.Load3(_109._m0.y * 192 + 160));
    _116._m5 = asfloat(_28.Load(_109._m0.y * 192 + 172));
    _116._m6 = _28.Load4(_109._m0.y * 192 + 176);
    _10 _119;
    _119._m0 = _31.Load4(_109._m0.x * 64 + 0);
    _119._m1 = asfloat(_31.Load4(_109._m0.x * 64 + 16));
    _119._m2 = asfloat(_31.Load4(_109._m0.x * 64 + 32));
    _119._m3 = asfloat(_31.Load4(_109._m0.x * 64 + 48));
    if (_102 >= _119._m0.w)
    {
        _95._m0 = float4(0.0f, 0.0f, 2.0f, 1.0f);
        gl_Position = _95._m0;
        gl_Position.y = -gl_Position.y;
        return;
    }
    uint _136 = (_119._m0.y + (_102 * 3u)) + _54(uint(gl_VertexIndex), 3u);
    uint _146 = _119._m0.x + ((_37.Load(_43(_136, 4u) * 4 + 0) >> (_54(_136, 4u) * 8u)) & 255u);
    _98 = mul(mul(float4(float3(asfloat(_40.Load((_34.Load(_146 * 4 + 0) * 3u) * 4 + 0)), asfloat(_40.Load(((_34.Load(_146 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_40.Load(((_34.Load(_146 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _116._m0), _19_m0._m0);
    _98.z += 0.001000000047497451305389404296875f;
    _95._m0 = _98;
    gl_Position = _95._m0;
    gl_Position.y = -gl_Position.y;
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    gl_VertexIndex = int(stage_input.gl_VertexIndex);
    gl_InstanceIndex = int(stage_input.gl_InstanceIndex);
    vert_main();
    SPIRV_Cross_Output stage_output;
    stage_output.gl_Position = gl_Position;
    return stage_output;
}
