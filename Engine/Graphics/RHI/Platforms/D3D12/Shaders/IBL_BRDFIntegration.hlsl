// Vulkan reference: IBL_BRDFIntegration.spv (cs_main).
#include "Common/TextureDimensions.hlsli"

RWTexture2D<float4> outputLUT : register(u0, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float2 hammersley(uint i, uint n)
{
    uint bits = 0u;
    bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 1431655765u) << 1u) | ((bits & 2863311530u) >> 1u);
    bits = ((bits & 858993459u) << 2u) | ((bits & 3435973836u) >> 2u);
    bits = ((bits & 252645135u) << 4u) | ((bits & 4042322160u) >> 4u);
    bits = ((bits & 16711935u) << 8u) | ((bits & 4278255360u) >> 8u);
    return float2(float(i) / float(n), float(bits) * 2.3283064e-10f);
}

float3 importanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float _87 = roughness * roughness;
    float _89 = 6.2831855f * xi.x;
    float _98 = sqrt((1.0f - xi.y) / (1.0f + (((_87 * _87) - 1.0f) * xi.y)));
    float _101 = sqrt(1.0f - (_98 * _98));
    return float3(cos(_89) * _101, sin(_89) * _101, _98);
}

float geometrySchlickGGX_IBL(float NdotV, float a)
{
    float _135 = (a * a) / 2.0f;
    return NdotV / ((NdotV * (1.0f - _135)) + _135);
}

float geometrySmith_IBL(float3 n, float3 v, float3 l, float a)
{
    return geometrySchlickGGX_IBL(max(dot(n, v), 0.0f), a) * geometrySchlickGGX_IBL(max(dot(n, l), 0.0f), a);
}

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    float A = 0.0f;
    uint i = 0u;
    bool _168 = false;
    float B = 0.0f;
    uint2 loop_bound = uint2(4294967295u, 4294967295u);

    uint2 _177 = imageDimensions(outputLUT);
    if (!(input.dispatchThreadID.x >= _177.x))
    {
        _168 = input.dispatchThreadID.y >= _177.y;
    }
    else
    {
        _168 = true;
    }
    if (_168)
    {
        return;
    }
    float _196 = (float(input.dispatchThreadID.x) + 0.5f) / float(_177.x);
    float _202 = (float(input.dispatchThreadID.y) + 0.5f) / float(_177.y);
    float3 _206 = float3(sqrt(1.0f - (_196 * _196)), 0.0f, _196);
    for (;;)
    {
        if (all(bool2(uint2(0u, 0u).x == loop_bound.x, uint2(0u, 0u).y == loop_bound.y)))
        {
            break;
        }
        loop_bound -= uint2(uint(loop_bound.y == 0u), 1u);
        if (!(i < 1024u))
        {
            break;
        }
        float3 _235 = importanceSampleGGX(hammersley(i, 1024u), float3(0.0f, 0.0f, 1.0f), _202);
        float3 _240 = normalize((_235 * (2.0f * dot(_206, _235))) - _206);
        float _246 = max(dot(_206, _235), 0.0f);
        if (max(_240.z, 0.0f) > 0.0f)
        {
            float _254 = (geometrySmith_IBL(float3(0.0f, 0.0f, 1.0f), _206, _240, _202) * _246) / ((max(_235.z, 0.0f) * _196) + 0.0001f);
            float _256 = pow(1.0f - _246, 5.0f);
            A += ((1.0f - _256) * _254);
            B += (_256 * _254);
        }
        i++;
        continue;
    }
    A /= float(1024u);
    B /= float(1024u);
    outputLUT[input.dispatchThreadID.xy] = float4(A, B, 0.0f, 0.0f);
}
