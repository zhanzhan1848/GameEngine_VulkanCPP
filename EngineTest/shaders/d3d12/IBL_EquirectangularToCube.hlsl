Texture2D<float4> _14 : register(t0, space0);
RWTexture2DArray<float4> _16 : register(u1, space0);

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

uint _38(uint _40, uint _41)
{
    return _40 / ((_41 == 0u) ? 1u : _41);
}

float2 _20(float3 _19)
{
    float2 _24 = 0.0f.xx;
    _24 = float2(atan2(_19.z, _19.x), asin(_19.y));
    _24 = (_24 * float2(0.159099996089935302734375f, 0.3183000087738037109375f)) + 0.5f.xx;
    return _24;
}

void comp_main()
{
    bool _70 = false;
    bool _73 = false;
    float3 _75 = 0.0f.xxx;
    uint _79_dummy_parameter;
    uint2 _79 = spvTextureSize(_14, 0u, _79_dummy_parameter);
    uint _81 = _38(_79.x, 4u);
    if (!(gl_GlobalInvocationID.x >= _81))
    {
        _70 = gl_GlobalInvocationID.y >= _81;
    }
    else
    {
        _70 = true;
    }
    if (!_70)
    {
        _73 = gl_GlobalInvocationID.z >= 6u;
    }
    else
    {
        _73 = true;
    }
    if (_73)
    {
        return;
    }
    float2 _108 = (((float2(gl_GlobalInvocationID.xy) + 0.5f.xx) / float2(float(_81), float(_81))) * 2.0f) - 1.0f.xx;
    switch (gl_GlobalInvocationID.z)
    {
        case 0u:
        {
            _75 = normalize(float3(1.0f, -_108.y, -_108.x));
            break;
        }
        case 1u:
        {
            _75 = normalize(float3(-1.0f, -_108.y, _108.x));
            break;
        }
        case 2u:
        {
            _75 = normalize(float3(_108.x, 1.0f, _108.y));
            break;
        }
        case 3u:
        {
            _75 = normalize(float3(_108.x, -1.0f, -_108.y));
            break;
        }
        case 4u:
        {
            _75 = normalize(float3(_108.x, -_108.y, 1.0f));
            break;
        }
        case 5u:
        {
            _75 = normalize(float3(-_108.x, -_108.y, -1.0f));
            break;
        }
        default:
        {
            _75 = float3(0.0f, 0.0f, 1.0f);
            break;
        }
    }
    _16[uint3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z)] = _14.Load(int3(min(max(uint2(clamp(_20(_75) * float2(_79), 0.0f.xx, 4294967040.0f.xx)), uint2(0u, 0u)), (_79 - uint2(1u, 1u))), 0));
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
