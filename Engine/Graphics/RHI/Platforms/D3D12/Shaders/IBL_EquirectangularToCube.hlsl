// Vulkan reference: IBL_EquirectangularToCube.spv (cs_main).
#include "Common/TextureDimensions.hlsli"

Texture2D<float4> equirectMap : register(t0, space0);
RWTexture2DArray<float4> outputCube : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

uint naga_div(uint lhs, uint rhs)
{
    return lhs / ((rhs == 0u) ? 1u : rhs);
}

float2 sampleSphericalMap(float3 v)
{
    float2 uv = 0.0f.xx;
    uv = float2(atan2(v.z, v.x), asin(v.y));
    uv = (uv * float2(0.1591f, 0.3183f)) + 0.5f.xx;
    return uv;
}

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    bool _71 = false;
    bool _74 = false;
    float3 dir = 0.0f.xxx;

    uint2 _80 = textureDimensions(equirectMap, 0u);
    uint _82 = naga_div(_80.x, 4u);
    if (!(input.dispatchThreadID.x >= _82))
    {
        _71 = input.dispatchThreadID.y >= _82;
    }
    else
    {
        _71 = true;
    }
    if (!_71)
    {
        _74 = input.dispatchThreadID.z >= 6u;
    }
    else
    {
        _74 = true;
    }
    if (_74)
    {
        return;
    }
    float2 _109 = (((float2(input.dispatchThreadID.xy) + 0.5f.xx) / float2(float(_82), float(_82))) * 2.0f) - 1.0f.xx;
    switch (input.dispatchThreadID.z)
    {
        case 0u:
        {
            dir = normalize(float3(1.0f, -_109.y, -_109.x));
            break;
        }
        case 1u:
        {
            dir = normalize(float3(-1.0f, -_109.y, _109.x));
            break;
        }
        case 2u:
        {
            dir = normalize(float3(_109.x, 1.0f, _109.y));
            break;
        }
        case 3u:
        {
            dir = normalize(float3(_109.x, -1.0f, -_109.y));
            break;
        }
        case 4u:
        {
            dir = normalize(float3(_109.x, -_109.y, 1.0f));
            break;
        }
        case 5u:
        {
            dir = normalize(float3(-_109.x, -_109.y, -1.0f));
            break;
        }
        default:
        {
            dir = float3(0.0f, 0.0f, 1.0f);
            break;
        }
    }
    outputCube[uint3(input.dispatchThreadID.xy, input.dispatchThreadID.z)] = equirectMap.Load(int3(min(max(uint2(clamp(sampleSphericalMap(dir) * float2(_80), 0.0f.xx, 4294967040.0f.xx)), uint2(0u, 0u)), (_80 - uint2(1u, 1u))), 0));
}
