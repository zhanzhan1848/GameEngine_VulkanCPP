// Vulkan reference: Nanite/HZBMip.spv (generate_hzb_mip_level).
#include "../Common/TextureDimensions.hlsli"

Texture2D<float4> src_mip : register(t0, space0);
RWTexture2D<float> dst_mip : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    bool _34 = false;

    uint2 _38 = imageDimensions(dst_mip);
    if (!(input.dispatchThreadID.x >= _38.x))
    {
        _34 = input.dispatchThreadID.y >= _38.y;
    }
    else
    {
        _34 = true;
    }
    if (_34)
    {
        return;
    }

    uint2 _52 = textureDimensions(src_mip, 0u);
    uint2 _54 = input.dispatchThreadID.xy * uint2(2u, 2u);
    uint2 _66 = uint2((_52.x > 0u) ? (_52.x - 1u) : 0u, (_52.y > 0u) ? (_52.y - 1u) : 0u);
    dst_mip[input.dispatchThreadID.xy] = float4(min(min(src_mip.Load(int3(min(_54, _66), 0)).x, src_mip.Load(int3(min((_54 + uint2(1u, 0u)), _66), 0)).x), min(src_mip.Load(int3(min((_54 + uint2(0u, 1u)), _66), 0)).x, src_mip.Load(int3(min((_54 + uint2(1u, 1u)), _66), 0)).x)), 0.0f, 0.0f, 0.0f).x;
}
