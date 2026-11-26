// Vulkan reference: Nanite/ShadowBlit.spv (shadow_depth_blit).
cbuffer resolution : register(b2, space0)
{
    uint2 resolution_1_m0 : packoffset(c0);
};

Texture2D<float4> src_depth : register(t0, space0);
RWTexture2D<float> dst_depth : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    bool _34 = false;
    if (!(input.dispatchThreadID.x >= resolution_1_m0.x))
    {
        _34 = input.dispatchThreadID.y >= resolution_1_m0.y;
    }
    else
    {
        _34 = true;
    }
    if (_34)
    {
        return;
    }
    dst_depth[int2(input.dispatchThreadID.xy)] = float4(src_depth.Load(int3(int2(input.dispatchThreadID.xy), 0)).x, 0.0f, 0.0f, 0.0f).x;
}
