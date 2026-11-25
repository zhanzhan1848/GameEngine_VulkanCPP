// Vulkan reference: Lumen/SurfaceCacheDilate.comp (surface_cache_dilate).
struct SurfaceCacheParams
{
    uint atlas_size;
    uint page_size;
    uint capture_budget_pages;
    uint max_cards;
    float update_distance;
    float importance_weight;
    uint max_lights_per_tile;
    uint indirect_rays_per_probe;
    float indirect_temporal_weight;
    float indirect_near_distance;
    uint lookup_count;
    uint _pad;
};

cbuffer ParamBlock : register(b2, space0)
{
    SurfaceCacheParams pb_params : packoffset(c0);
};

RWTexture2D<float> depth_in : register(u0, space0);
RWTexture2D<float> depth_out : register(u1, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;
    if (((gid.x >= pb_params.atlas_size) || (gid.y >= pb_params.atlas_size)))
    {
        return;
    }
    int2 coord = int2(gid);
    float center = depth_in[coord].xxxx.x;
    if (center > 0.0f)
    {
        depth_out[coord] = float4(center, 0.0f, 0.0f, 1.0f).x;
        return;
    }
    float min_depth = 10000000000.0f;
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if ((dx == 0) && (dy == 0))
            {
                continue;
            }
            int2 nb = coord + int2(dx, dy);
            if (((((nb.x < 0) || (nb.y < 0)) || (nb.x >= int(pb_params.atlas_size))) || (nb.y >= int(pb_params.atlas_size))))
            {
                continue;
            }
            float d = depth_in[nb].xxxx.x;
            if (d > 0.0f)
            {
                min_depth = min(min_depth, d);
            }
        }
    }
    depth_out[coord] = float4((min_depth < 10000000000.0f) ? min_depth : 0.0f, 0.0f, 0.0f, 1.0f).x;
}
