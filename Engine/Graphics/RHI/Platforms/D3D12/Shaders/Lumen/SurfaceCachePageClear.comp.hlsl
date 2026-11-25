// Vulkan reference: Lumen/SurfaceCachePageClear.comp (surface_cache_page_clear).
ByteAddressBuffer rb : register(t6, space0);
RWTexture2D<float4> lighting0 : register(u0, space0);
RWTexture2D<float4> lighting1 : register(u1, space0);
RWTexture2D<float4> lighting2 : register(u2, space0);
RWTexture2D<float4> nodirect0 : register(u3, space0);
RWTexture2D<float4> nodirect1 : register(u4, space0);
RWTexture2D<float4> nodirect2 : register(u5, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(256, 1, 1)]
void main(StageInput input)
{
    uint tid = input.dispatchThreadID.x;
    uint count = rb.Load(0);
    for (uint r = 0u; r < count; r++)
    {
        uint base = 1u + (r * 4u);
        uint area = rb.Load((base + 2u) * 4) * rb.Load((base + 3u) * 4);
        if (tid >= area)
        {
            tid -= area;
            continue;
        }
        uint lx = tid % rb.Load((base + 2u) * 4);
        uint ly = tid / rb.Load((base + 2u) * 4);
        int2 uv = int2(int(rb.Load((base + 0u) * 4) + lx), int(rb.Load((base + 1u) * 4) + ly));
        lighting0[uv] = 0.0f.xxxx;
        lighting1[uv] = 0.0f.xxxx;
        lighting2[uv] = 0.0f.xxxx;
        nodirect0[uv] = 0.0f.xxxx;
        nodirect1[uv] = 0.0f.xxxx;
        nodirect2[uv] = 0.0f.xxxx;
        return;
    }
}
