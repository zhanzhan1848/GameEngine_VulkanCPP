Texture2D<float4> _11 : register(t0, space0);
RWTexture2D<float> _13 : register(u1, space0);

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

uint2 spvImageSize(RWTexture2D<float> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

void comp_main()
{
    bool _33 = false;
    uint _37_dummy_parameter;
    uint2 _37 = spvImageSize(_13, _37_dummy_parameter);
    if (!(gl_GlobalInvocationID.x >= _37.x))
    {
        _33 = gl_GlobalInvocationID.y >= _37.y;
    }
    else
    {
        _33 = true;
    }
    if (_33)
    {
        return;
    }
    uint _51_dummy_parameter;
    uint2 _51 = spvTextureSize(_11, uint(0), _51_dummy_parameter);
    uint2 _53 = gl_GlobalInvocationID.xy * uint2(2u, 2u);
    uint2 _65 = uint2((_51.x > 0u) ? (_51.x - 1u) : 0u, (_51.y > 0u) ? (_51.y - 1u) : 0u);
    _13[gl_GlobalInvocationID.xy] = float4(min(min(_11.Load(int3(min(_53, _65), 0)).x, _11.Load(int3(min((_53 + uint2(1u, 0u)), _65), 0)).x), min(_11.Load(int3(min((_53 + uint2(0u, 1u)), _65), 0)).x, _11.Load(int3(min((_53 + uint2(1u, 1u)), _65), 0)).x)), 0.0f, 0.0f, 0.0f).x;
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
