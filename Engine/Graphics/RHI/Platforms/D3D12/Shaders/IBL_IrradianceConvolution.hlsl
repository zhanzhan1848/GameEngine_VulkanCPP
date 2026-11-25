// Vulkan reference: IBL_IrradianceConvolution.spv (cs_main).
struct IrradianceParams
{
    uint faceSize;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

cbuffer params : register(b3, space0)
{
    IrradianceParams params_1_m0 : packoffset(c0);
};

TextureCube<float4> envMap : register(t0, space0);
RWTexture2DArray<float4> outputCube : register(u1, space0);
SamplerState smp : register(s2, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    float theta = 0.0f;
    float3 irradiance = 0.0f.xxx;
    float3 N = 0.0f.xxx;
    bool _189 = false;
    float nrSamples = 0.0f;
    float3 up = float3(0.0f, 1.0f, 0.0f);
    bool _192 = false;
    float phi = 0.0f;
    float3 right = 0.0f.xxx;
    float v = 0.0f;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);
    uint2 loop_bound_1 = uint2(4294967295u, 4294967295u);
    if (!(input.dispatchThreadID.x >= params_1_m0.faceSize))
    {
        _189 = input.dispatchThreadID.y >= params_1_m0.faceSize;
    }
    else
    {
        _189 = true;
    }
    if (!_189)
    {
        _192 = input.dispatchThreadID.z >= 6u;
    }
    else
    {
        _192 = true;
    }
    if (_192)
    {
        return;
    }
    float2 _243 = (float2(input.dispatchThreadID.xy) + 0.5f.xx) / float2(float(params_1_m0.faceSize), float(params_1_m0.faceSize));
    float _246 = (2.0f * _243.x) - 1.0f;
    v = -((2.0f * _243.y) - 1.0f);
    switch (input.dispatchThreadID.z)
    {
        case 0u:
        {
            N = float3(1.0f, v, -_246);
            break;
        }
        case 1u:
        {
            N = float3(-1.0f, v, _246);
            break;
        }
        case 2u:
        {
            N = float3(_246, 1.0f, -v);
            break;
        }
        case 3u:
        {
            N = float3(_246, -1.0f, v);
            break;
        }
        case 4u:
        {
            N = float3(_246, v, 1.0f);
            break;
        }
        case 5u:
        {
            N = float3(-_246, v, -1.0f);
            break;
        }
        default:
        {
            N = float3(0.0f, 0.0f, 1.0f);
            break;
        }
    }
    N = normalize(N);
    right = cross(up, N);
    if (length(right) < 0.001f)
    {
        up = float3(1.0f, 0.0f, 0.0f);
        right = cross(up, N);
    }
    right = normalize(right);
    up = normalize(cross(N, right));
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(phi < 6.2831855f))
        {
            break;
        }
        theta = 0.0f;
        for (;;)
        {
            if (all(bool2(uint2(0u, 0u).x == loop_bound_1.x, uint2(0u, 0u).y == loop_bound_1.y)))
            {
                break;
            }
            loop_bound_1 -= uint2(uint(loop_bound_1.y == 0u), 1u);
            if (!(theta < 1.5707964f))
            {
                break;
            }
            float3 _353 = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            irradiance += ((envMap.SampleLevel(smp, ((right * _353.x) + (up * _353.y)) + (N * _353.z), 0.0f).xyz * cos(theta)) * sin(theta));
            nrSamples += 1.0f;
            theta += 0.025f;
            continue;
        }
        phi += 0.025f;
        continue;
    }
    irradiance = (irradiance * 3.1415927f) * (1.0f / nrSamples);
    outputCube[uint3(input.dispatchThreadID.xy, input.dispatchThreadID.z)] = float4(irradiance, 1.0f);
}
