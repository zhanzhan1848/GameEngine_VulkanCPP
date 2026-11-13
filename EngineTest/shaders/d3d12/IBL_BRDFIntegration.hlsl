RWTexture2D<float4> _12 : register(u0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvImageSize(RWTexture2D<float4> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

float2 _17(uint _15, uint _16)
{
    uint _33 = 0u;
    _33 = _15;
    _33 = (_33 << 16u) | (_33 >> 16u);
    _33 = ((_33 & 1431655765u) << 1u) | ((_33 & 2863311530u) >> 1u);
    _33 = ((_33 & 858993459u) << 2u) | ((_33 & 3435973836u) >> 2u);
    _33 = ((_33 & 252645135u) << 4u) | ((_33 & 4042322160u) >> 4u);
    _33 = ((_33 & 16711935u) << 8u) | ((_33 & 4278255360u) >> 8u);
    return float2(float(_15) / float(_16), float(_33) * 2.3283064365386962890625e-10f);
}

float3 _81(float2 _78, float3 _79, float _80)
{
    float _86 = _80 * _80;
    float _88 = 6.283185482025146484375f * _78.x;
    float _97 = sqrt((1.0f - _78.y) / (1.0f + (((_86 * _86) - 1.0f) * _78.y)));
    float _100 = sqrt(1.0f - (_97 * _97));
    return float3(cos(_88) * _100, sin(_88) * _100, _97);
}

float _129(float _127, float _128)
{
    float _134 = (_128 * _128) / 2.0f;
    return _127 / ((_127 * (1.0f - _134)) + _134);
}

float _144(float3 _140, float3 _141, float3 _142, float _143)
{
    return _129(max(dot(_140, _141), 0.0f), _143) * _129(max(dot(_140, _142), 0.0f), _143);
}

void comp_main()
{
    float _170 = 0.0f;
    uint _173 = 0u;
    bool _167 = false;
    float _172 = 0.0f;
    uint2 _215 = uint2(4294967295u, 4294967295u);
    uint _176_dummy_parameter;
    uint2 _176 = spvImageSize(_12, _176_dummy_parameter);
    if (!(gl_GlobalInvocationID.x >= _176.x))
    {
        _167 = gl_GlobalInvocationID.y >= _176.y;
    }
    else
    {
        _167 = true;
    }
    if (_167)
    {
        return;
    }
    float _195 = (float(gl_GlobalInvocationID.x) + 0.5f) / float(_176.x);
    float _201 = (float(gl_GlobalInvocationID.y) + 0.5f) / float(_176.y);
    float3 _205 = float3(sqrt(1.0f - (_195 * _195)), 0.0f, _195);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == _215.x, uint2(0u, 0u).y == _215.y)))
        {
            break;
        }
        _215 -= uint2(uint(_215.y == 0u), 1u);
        if (!(_173 < 1024u))
        {
            break;
        }
        float3 _234 = _81(_17(_173, 1024u), float3(0.0f, 0.0f, 1.0f), _201);
        float3 _239 = normalize((_234 * (2.0f * dot(_205, _234))) - _205);
        float _245 = max(dot(_205, _234), 0.0f);
        if (max(_239.z, 0.0f) > 0.0f)
        {
            float _253 = (_144(float3(0.0f, 0.0f, 1.0f), _205, _239, _201) * _245) / ((max(_234.z, 0.0f) * _195) + 9.9999997473787516355514526367188e-05f);
            float _255 = pow(1.0f - _245, 5.0f);
            _170 += ((1.0f - _255) * _253);
            _172 += (_255 * _253);
        }
        _173++;
        continue;
    }
    _170 /= float(1024u);
    _172 /= float(1024u);
    _12[gl_GlobalInvocationID.xy] = float4(_170, _172, 0.0f, 0.0f);
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
