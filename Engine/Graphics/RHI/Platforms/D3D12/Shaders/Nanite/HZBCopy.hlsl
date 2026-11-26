// Vulkan reference: Nanite/HZBCopy.spv (copy_depth_to_hzb_mip0).
#include "../Common/TextureDimensions.hlsli"

Texture2D<float4> src_depth : register(t0, space0);
RWTexture2D<float> dst_hzb : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(16, 16, 1)]
void main(StageInput input)
{
    bool _29 = false;
    float value = 0.0f;
    bool _35 = false;

    uint2 _40 = textureDimensions(src_depth, 0u);
    if (!(input.dispatchThreadID.x >= _40.x))
    {
        _29 = input.dispatchThreadID.y >= _40.y;
    }
    else
    {
        _29 = true;
    }
    if (_29)
    {
        return;
    }
    value = src_depth.Load(int3(input.dispatchThreadID.xy, 0)).x;
    if (value >= 0.0f)
    {
        _35 = value <= 1.0f;
    }
    else
    {
        _35 = false;
    }
    if (!_35)
    {
        value = 1.0f;
    }
    dst_hzb[input.dispatchThreadID.xy] = float4(value, 0.0f, 0.0f, 0.0f).x;
}
