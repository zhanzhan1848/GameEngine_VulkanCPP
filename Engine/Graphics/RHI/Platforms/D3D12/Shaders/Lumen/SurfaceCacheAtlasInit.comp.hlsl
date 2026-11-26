// Vulkan reference: Lumen/SurfaceCacheAtlasInit.comp (surface_cache_atlas_init).
#include "../Common/TextureDimensions.hlsli"

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

struct FlattenedLightingParams
{
    SurfaceCacheParams sc_params;
    uint total_texels;
    uint card_count;
    uint light_count;
    uint opensky_v2;
    float4 ddgi_origin_spacing;
    uint4 ddgi_counts_valid;
    uint4 radiosity_gate;
    float probe_feed_bootstrap;
    uint radiosity_solo;
    uint page_table_enabled;
};

struct CardDispatchInfo
{
    uint texel_offset;
    uint texel_count;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint card_index;
    uint _pad1;
    uint _pad2;
};

cbuffer ParamBlock : register(b4, space0)
{
    FlattenedLightingParams pb_params : packoffset(c0);
};

ByteAddressBuffer cd : register(t5, space0);
RWByteAddressBuffer atlasTriangles : register(u6, space0);
RWTexture2D<float> depth_atlas : register(u2, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u0, space0);
RWTexture2D<float2> normal_atlas : register(u1, space0);
RWTexture2D<float4> emissive_atlas : register(u3, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

[numthreads(256, 1, 1)]
void main(StageInput input)
{
    uint tid = input.dispatchThreadID.x;
    bool legacySurface = (pb_params.sc_params._pad & 1u) != 0u;

    int2 dims = int2(imageDimensions(depth_atlas));
    int2 atlas_uv;
    if (legacySurface)
    {
        if (tid >= pb_params.total_texels)
        {
            return;
        }
        uint lo = 0u;
        uint hi = pb_params.card_count;
        while (lo < hi)
        {
            uint mid = (lo + hi) >> 1u;
            if ((cd.Load(mid * 32) + cd.Load(mid * 32 + 4)) <= tid)
            {
                lo = mid + 1u;
            }
            else
            {
                hi = mid;
            }
        }
        if (lo >= pb_params.card_count)
        {
            return;
        }
        uint local_texel = tid - cd.Load(lo * 32);
        uint res = cd.Load(lo * 32 + 8);
        atlas_uv = int2(int(cd.Load(lo * 32 + 12) + (local_texel % res)), int(cd.Load(lo * 32 + 16) + (local_texel / res)));
    }
    else
    {
        if (tid >= (uint(dims.x) * uint(dims.y)))
        {
            return;
        }
        atlas_uv = int2(int(tid % uint(dims.x)), int(tid / uint(dims.x)));
    }

    atlasTriangles.Store(((uint(atlas_uv.y) * uint(int2(imageDimensions(depth_atlas)).x)) + uint(atlas_uv.x)) * 4 + 0, 4294967295u);
    albedo_atlas[atlas_uv] = 0.0f.xxxx;
    normal_atlas[atlas_uv] = float4(0.5f, 0.5f, 0.0f, 1.0f).xy;
    depth_atlas[atlas_uv] = float4(-1.0f, 0.0f, 0.0f, 1.0f).x;
    bool4 _202 = legacySurface.xxxx;
    emissive_atlas[atlas_uv] = float4(_202.x ? float4(0.0f, 0.0f, 0.0f, 1.0f).x : 0.0f.xxxx.x, _202.y ? float4(0.0f, 0.0f, 0.0f, 1.0f).y : 0.0f.xxxx.y, _202.z ? float4(0.0f, 0.0f, 0.0f, 1.0f).z : 0.0f.xxxx.z, _202.w ? float4(0.0f, 0.0f, 0.0f, 1.0f).w : 0.0f.xxxx.w);
}
