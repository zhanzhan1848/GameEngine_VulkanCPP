Texture2D<float4> _10 : register(t0, space0);
RWTexture2D<float> _12 : register(u1, space0);

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
    bool _28 = false;
    float _31 = 0.0f;
    bool _34 = false;
    uint _39_dummy_parameter;
    uint2 _39 = spvTextureSize(_10, 0u, _39_dummy_parameter);
    if (!(gl_GlobalInvocationID.x >= _39.x))
    {
        _28 = gl_GlobalInvocationID.y >= _39.y;
    }
    else
    {
        _28 = true;
    }
    if (_28)
    {
        return;
    }
    _31 = _10.Load(int3(gl_GlobalInvocationID.xy, 0)).x;
    if (_31 >= 0.0f)
    {
        _34 = _31 <= 1.0f;
    }
    else
    {
        _34 = false;
    }
    if (!_34)
    {
        _31 = 1.0f;
    }
    _12[gl_GlobalInvocationID.xy] = float4(_31, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
