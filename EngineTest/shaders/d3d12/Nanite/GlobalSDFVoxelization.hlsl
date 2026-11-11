struct _7
{
    float4 _m0;
    uint4 _m1;
};

struct _9
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

cbuffer _24_23 : register(b1, space0)
{
    _7 _23_m0 : packoffset(c0);
};

ByteAddressBuffer _26 : register(t2, space0);
ByteAddressBuffer _29 : register(t3, space0);
ByteAddressBuffer _32 : register(t4, space0);
ByteAddressBuffer _35 : register(t5, space0);
ByteAddressBuffer _38 : register(t6, space0);
ByteAddressBuffer _41 : register(t7, space0);
RWTexture3D<float> _21 : register(u0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint _193(uint _195, uint _196)
{
    return _195 / ((_196 == 0u) ? 1u : _196);
}

uint _203(uint _204, uint _205)
{
    return _204 % ((_205 == 0u) ? 1u : _205);
}

float _49(float3 _45, float3 _46, float3 _47, float3 _48)
{
    bool _67 = false;
    bool _61 = false;
    bool _54 = false;
    bool _69 = false;
    bool _63 = false;
    bool _57 = false;
    bool _71 = false;
    bool _65 = false;
    bool _59 = false;
    float3 _74 = _47 - _46;
    float3 _75 = _48 - _46;
    float3 _76 = _45 - _46;
    float _77 = dot(_74, _76);
    float _78 = dot(_75, _76);
    if (_77 <= 0.0f)
    {
        _54 = _78 <= 0.0f;
    }
    else
    {
        _54 = false;
    }
    if (_54)
    {
        return length(_76);
    }
    float3 _88 = _45 - _47;
    float _89 = dot(_74, _88);
    float _90 = dot(_75, _88);
    if (_89 >= 0.0f)
    {
        _57 = _90 <= _89;
    }
    else
    {
        _57 = false;
    }
    if (_57)
    {
        return length(_88);
    }
    float _102 = (_77 * _90) - (_89 * _78);
    if (_102 <= 0.0f)
    {
        _59 = _77 >= 0.0f;
    }
    else
    {
        _59 = false;
    }
    if (_59)
    {
        _61 = _89 <= 0.0f;
    }
    else
    {
        _61 = false;
    }
    if (_61)
    {
        return length(_76 + (_74 * (_77 / (_77 - _89))));
    }
    float3 _121 = _45 - _48;
    float _122 = dot(_74, _121);
    float _123 = dot(_75, _121);
    if (_123 >= 0.0f)
    {
        _63 = _122 <= _123;
    }
    else
    {
        _63 = false;
    }
    if (_63)
    {
        return length(_121);
    }
    float _135 = (_122 * _78) - (_77 * _123);
    if (_135 <= 0.0f)
    {
        _65 = _78 >= 0.0f;
    }
    else
    {
        _65 = false;
    }
    if (_65)
    {
        _67 = _123 <= 0.0f;
    }
    else
    {
        _67 = false;
    }
    if (_67)
    {
        return length(_76 + (_75 * (_78 / (_78 - _123))));
    }
    float _156 = (_89 * _123) - (_122 * _90);
    if (_156 <= 0.0f)
    {
        _69 = (_90 - _89) >= 0.0f;
    }
    else
    {
        _69 = false;
    }
    if (_69)
    {
        _71 = (_122 - _123) >= 0.0f;
    }
    else
    {
        _71 = false;
    }
    if (_71)
    {
        return length((_47 + ((_48 - _47) * ((_90 - _89) / ((_90 - _89) + (_122 - _123))))) - _45);
    }
    float _184 = 1.0f / ((_156 + _135) + _102);
    return length(((_76 + (_74 * (_135 * _184))) + (_75 * (_102 * _184))) - _45);
}

void comp_main()
{
    uint _242 = 0u;
    float _240 = 1000000.0f;
    uint2 _282 = uint2(4294967295u, 4294967295u);
    uint2 _341 = uint2(4294967295u, 4294967295u);
    uint2 _399 = uint2(4294967295u, 4294967295u);
    if (any(bool3(gl_GlobalInvocationID.x >= _23_m0._m1.xyz.x, gl_GlobalInvocationID.y >= _23_m0._m1.xyz.y, gl_GlobalInvocationID.z >= _23_m0._m1.xyz.z)))
    {
        return;
    }
    float3 _271 = _23_m0._m0.xyz + ((float3(gl_GlobalInvocationID) + 0.5f.xxx) * _23_m0._m0.w);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == _282.x, uint2(0u, 0u).y == _282.y)))
        {
            break;
        }
        _282 -= uint2(uint(_282.y == 0u), 1u);
        if (!(_242 < _23_m0._m1.w))
        {
            break;
        }
        if (_240 <= 0.0f)
        {
            break;
        }
        _9 _306;
        _306._m0 = asfloat(uint4x4(_41.Load4(_242 * 192 + 0), _41.Load4(_242 * 192 + 16), _41.Load4(_242 * 192 + 32), _41.Load4(_242 * 192 + 48)));
        _306._m1 = asfloat(uint4x4(_41.Load4(_242 * 192 + 64), _41.Load4(_242 * 192 + 80), _41.Load4(_242 * 192 + 96), _41.Load4(_242 * 192 + 112)));
        _306._m2 = _41.Load4(_242 * 192 + 128);
        _306._m3 = _41.Load4(_242 * 192 + 144);
        _306._m4 = asfloat(_41.Load(_242 * 192 + 160));
        _306._m5 = asfloat(_41.Load(_242 * 192 + 164));
        _306._m6 = asfloat(_41.Load(_242 * 192 + 168));
        _306._m7 = _41.Load(_242 * 192 + 172);
        _306._m8 = asfloat(_41.Load(_242 * 192 + 176));
        _306._m9 = _41.Load(_242 * 192 + 180);
        _306._m10 = _41.Load(_242 * 192 + 184);
        _306._m11 = _41.Load(_242 * 192 + 188);
        if ((length(_271 - float3(_306._m4, _306._m5, _306._m6)) - _306._m8) > (_23_m0._m0.w * 4.0f))
        {
            uint _510 = _242;
            _242 = _510 + 1u;
            continue;
        }
        float _336 = max(max(length(_306._m0[0].xyz), length(_306._m0[1].xyz)), length(_306._m0[2].xyz));
        uint _244 = 0u;
        for (;;)
        {
            if (all(bool2(uint2(0u, 0u).x == _341.x, uint2(0u, 0u).y == _341.y)))
            {
                break;
            }
            _341 -= uint2(uint(_341.y == 0u), 1u);
            if (!(_244 < _306._m2.w))
            {
                break;
            }
            _11 _362;
            _362._m0 = _38.Load4((_306._m2.z + _244) * 16 + 0);
            _10 _367;
            _367._m0 = _29.Load4(_362._m0.x * 64 + 0);
            _367._m1 = asfloat(_29.Load4(_362._m0.x * 64 + 16));
            _367._m2 = asfloat(_29.Load4(_362._m0.x * 64 + 32));
            _367._m3 = asfloat(_29.Load4(_362._m0.x * 64 + 48));
            if ((length(_271 - mul(float4(float3(_367._m2.w, _367._m3.x, _367._m3.y), 1.0f), _306._m0).xyz) - (_367._m3.z * _336)) > (_23_m0._m0.w * 2.0f))
            {
                uint _508 = _244;
                _244 = _508 + 1u;
                continue;
            }
            uint _246 = 0u;
            for (;;)
            {
                if (all(bool2(uint2(0u, 0u).x == _399.x, uint2(0u, 0u).y == _399.y)))
                {
                    break;
                }
                _399 -= uint2(uint(_399.y == 0u), 1u);
                if (!(_246 < _367._m0.w))
                {
                    break;
                }
                uint _418 = _367._m0.y + (_246 * 3u);
                uint _445 = _367._m0.x + ((_35.Load(_193(_418, 4u) * 4 + 0) >> (_203(_418, 4u) * 8u)) & 255u);
                uint _448 = _367._m0.x + ((_35.Load(_193(_418 + 1u, 4u) * 4 + 0) >> (_203(_418 + 1u, 4u) * 8u)) & 255u);
                uint _451 = _367._m0.x + ((_35.Load(_193(_418 + 2u, 4u) * 4 + 0) >> (_203(_418 + 2u, 4u) * 8u)) & 255u);
                _240 = min(_240, _49(_271, mul(float4(float3(asfloat(_26.Load((_32.Load(_445 * 4 + 0) * 3u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_445 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_445 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _306._m0).xyz, mul(float4(float3(asfloat(_26.Load((_32.Load(_448 * 4 + 0) * 3u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_448 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_448 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _306._m0).xyz, mul(float4(float3(asfloat(_26.Load((_32.Load(_451 * 4 + 0) * 3u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_451 * 4 + 0) * 3u) + 1u) * 4 + 0)), asfloat(_26.Load(((_32.Load(_451 * 4 + 0) * 3u) + 2u) * 4 + 0))), 1.0f), _306._m0).xyz));
                _246++;
                continue;
            }
            uint _508 = _244;
            _244 = _508 + 1u;
            continue;
        }
        uint _510 = _242;
        _242 = _510 + 1u;
        continue;
    }
    _21[int3(gl_GlobalInvocationID)] = float4(_240, 0.0f, 0.0f, 0.0f).x;
}

[numthreads(4, 4, 4)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
