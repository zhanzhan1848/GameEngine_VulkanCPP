struct _6
{
    row_major float4x4 _m0;
    row_major float4x4 _m1;
    float _m2;
    float _m3;
    float _m4;
    float _m5;
};

cbuffer _20_19 : register(b4, space0)
{
    _6 _19_m0 : packoffset(c0);
};

Texture2D<float4> _13 : register(t0, space0);
Texture2D<float4> _15 : register(t1, space0);
Texture2D<float4> _16 : register(t2, space0);
RWTexture2D<float4> _17 : register(u3, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    bool _36 = false;
    uint2 _53 = uint2(uint(clamp(_19_m0._m2, 0.0f, 4294967040.0f)), uint(clamp(_19_m0._m3, 0.0f, 4294967040.0f)));
    if (!(gl_GlobalInvocationID.x >= _53.x))
    {
        _36 = gl_GlobalInvocationID.y >= _53.y;
    }
    else
    {
        _36 = true;
    }
    if (_36)
    {
        return;
    }
    uint2 _69 = uint2(gl_GlobalInvocationID.xy);
    _17[_69] = _13.Load(int3(_69, 0));
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
