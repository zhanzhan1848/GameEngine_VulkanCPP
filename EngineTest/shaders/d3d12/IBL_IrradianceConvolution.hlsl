struct _10
{
    uint _m0;
    uint _m1;
    uint _m2;
    uint _m3;
};

cbuffer _22_21 : register(b3, space0)
{
    _10 _21_m0 : packoffset(c0);
};

TextureCube<float4> _15 : register(t0, space0);
RWTexture2DArray<float4> _17 : register(u1, space0);
SamplerState _19 : register(s2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    float _205 = 0.0f;
    float3 _202 = 0.0f.xxx;
    float3 _196 = 0.0f.xxx;
    bool _188 = false;
    float _203 = 0.0f;
    float3 _199 = float3(0.0f, 1.0f, 0.0f);
    bool _191 = false;
    float _204 = 0.0f;
    float3 _200 = 0.0f.xxx;
    float _193 = 0.0f;
    uint2 _302 = uint2(4294967295u, 4294967295u);
    uint2 _323 = uint2(4294967295u, 4294967295u);
    if (!(gl_GlobalInvocationID.x >= _21_m0._m0))
    {
        _188 = gl_GlobalInvocationID.y >= _21_m0._m0;
    }
    else
    {
        _188 = true;
    }
    if (!_188)
    {
        _191 = gl_GlobalInvocationID.z >= 6u;
    }
    else
    {
        _191 = true;
    }
    if (_191)
    {
        return;
    }
    float2 _242 = (float2(gl_GlobalInvocationID.xy) + 0.5f.xx) / float2(float(_21_m0._m0), float(_21_m0._m0));
    float _245 = (2.0f * _242.x) - 1.0f;
    _193 = -((2.0f * _242.y) - 1.0f);
    switch (gl_GlobalInvocationID.z)
    {
        case 0u:
        {
            _196 = float3(1.0f, _193, -_245);
            break;
        }
        case 1u:
        {
            _196 = float3(-1.0f, _193, _245);
            break;
        }
        case 2u:
        {
            _196 = float3(_245, 1.0f, -_193);
            break;
        }
        case 3u:
        {
            _196 = float3(_245, -1.0f, _193);
            break;
        }
        case 4u:
        {
            _196 = float3(_245, _193, 1.0f);
            break;
        }
        case 5u:
        {
            _196 = float3(-_245, _193, -1.0f);
            break;
        }
        default:
        {
            _196 = float3(0.0f, 0.0f, 1.0f);
            break;
        }
    }
    _196 = normalize(_196);
    _200 = cross(_199, _196);
    if (length(_200) < 0.001000000047497451305389404296875f)
    {
        _199 = float3(1.0f, 0.0f, 0.0f);
        _200 = cross(_199, _196);
    }
    _200 = normalize(_200);
    _199 = normalize(cross(_196, _200));
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == _302.x, uint2(0u, 0u).y == _302.y)))
        {
            break;
        }
        _302 -= uint2(uint(_302.y == 0u), 1u);
        if (!(_204 < 6.283185482025146484375f))
        {
            break;
        }
        _205 = 0.0f;
        for (;;)
        {
            if (all(bool2(uint2(0u, 0u).x == _323.x, uint2(0u, 0u).y == _323.y)))
            {
                break;
            }
            _323 -= uint2(uint(_323.y == 0u), 1u);
            if (!(_205 < 1.57079637050628662109375f))
            {
                break;
            }
            float3 _352 = float3(sin(_205) * cos(_204), sin(_205) * sin(_204), cos(_205));
            _202 += ((_15.SampleLevel(_19, ((_200 * _352.x) + (_199 * _352.y)) + (_196 * _352.z), 0.0f).xyz * cos(_205)) * sin(_205));
            _203 += 1.0f;
            _205 += 0.02500000037252902984619140625f;
            continue;
        }
        _204 += 0.02500000037252902984619140625f;
        continue;
    }
    _202 = (_202 * 3.1415927410125732421875f) * (1.0f / _203);
    _17[uint3(gl_GlobalInvocationID.xy, gl_GlobalInvocationID.z)] = float4(_202, 1.0f);
}

[numthreads(16, 16, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
