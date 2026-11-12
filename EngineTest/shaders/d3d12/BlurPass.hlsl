struct _6
{
    uint _m0;
    uint _m1;
    int _m2;
    float _m3;
    uint _m4;
    uint _m5;
    uint _m6;
    uint _m7;
};

cbuffer _19_18 : register(b2, space0)
{
    _6 _18_m0 : packoffset(c0);
};

Texture2D<float4> _14 : register(t0, space0);
RWTexture2D<float4> _16 : register(u1, space0);

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
    bool _42 = false;
    float4 _45 = 0.0f.xxxx;
    uint _49_dummy_parameter;
    uint2 _49 = spvTextureSize(_14, 0u, _49_dummy_parameter);
    if (!(gl_GlobalInvocationID.x >= _49.x))
    {
        _42 = gl_GlobalInvocationID.y >= _49.y;
    }
    else
    {
        _42 = true;
    }
    if (_42)
    {
        return;
    }
    uint2 _65 = uint2(gl_GlobalInvocationID.xy);
    bool2 _74 = (_18_m0._m5 == 1u).xx;
    int2 _72 = int2(_74.x ? int2(0, 1).x : int2(1, 0).x, _74.y ? int2(0, 1).y : int2(1, 0).y);
    _45 = _14.Load(int3(_65, 0)) * 0.2270270287990570068359375f;
    _45 += (_14.Load(int3(_65 + uint2(_72), 0)) * 0.3162162303924560546875f);
    _45 += (_14.Load(int3(_65 - uint2(_72), 0)) * 0.3162162303924560546875f);
    _45 += (_14.Load(int3(_65 + (uint2(_72) * uint2(2u, 2u)), 0)) * 0.0702702701091766357421875f);
    _45 += (_14.Load(int3(_65 - (uint2(_72) * uint2(2u, 2u)), 0)) * 0.0702702701091766357421875f);
    _16[_65] = _45;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
