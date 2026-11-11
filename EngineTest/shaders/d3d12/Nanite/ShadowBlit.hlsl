cbuffer _16_15 : register(b2, space0)
{
    uint2 _15_m0 : packoffset(c0);
};

Texture2D<float4> _11 : register(t0, space0);
RWTexture2D<float> _13 : register(u1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    bool _33 = false;
    if (!(gl_GlobalInvocationID.x >= _15_m0.x))
    {
        _33 = gl_GlobalInvocationID.y >= _15_m0.y;
    }
    else
    {
        _33 = true;
    }
    if (_33)
    {
        return;
    }
    _13[int2(gl_GlobalInvocationID.xy)] = float4(_11.Load(int3(int2(gl_GlobalInvocationID.xy), 0)).x, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
