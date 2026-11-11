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

struct LightCullParams
{
    SurfaceCacheParams sc_params;
    uint tiles_x;
    uint tile_size;
    uint light_count;
    uint tiles_y;
};

struct LightInfo
{
    float4 position;
    float4 color;
    float4 direction;
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer LightCullParamsBlock : register(b2, space0)
{
    LightCullParams lcp_params : packoffset(c0);
};

RWByteAddressBuffer tile_light_assignment : register(u4, space0);
ByteAddressBuffer lights : register(t3, space0);
RWTexture2D<float> depth_atlas : register(u0, space0);
RWTexture2D<float2> normal_atlas : register(u1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _52 = n;
    float3 _56 = n;
    float2 _58 = _56.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _52.xy));
    n.x = _58.x;
    n.y = _58.y;
    return normalize(n);
}

void comp_main()
{
    uint tile_x = gl_GlobalInvocationID.x;
    uint tile_y = gl_GlobalInvocationID.y;
    bool _91 = tile_x >= lcp_params.tiles_x;
    bool _100;
    if (!_91)
    {
        _100 = tile_y >= lcp_params.tiles_y;
    }
    else
    {
        _100 = _91;
    }
    if (_100)
    {
        return;
    }
    uint tile_idx = (tile_y * lcp_params.tiles_x) + tile_x;
    uint tile_size = lcp_params.tile_size;
    uint2 tile_origin = uint2(tile_x * tile_size, tile_y * tile_size);
    float min_depth = 10000000000.0f;
    float max_depth = 0.0f;
    for (uint dy = 0u; dy < tile_size; dy += 2u)
    {
        for (uint dx = 0u; dx < tile_size; dx += 2u)
        {
            uint2 texel = tile_origin + uint2(dx, dy);
            bool _156 = texel.x >= lcp_params.sc_params.atlas_size;
            bool _165;
            if (!_156)
            {
                _165 = texel.y >= lcp_params.sc_params.atlas_size;
            }
            else
            {
                _165 = _156;
            }
            if (_165)
            {
                continue;
            }
            float d = depth_atlas[int2(texel)].xxxx.x;
            if (d > 0.0f)
            {
                min_depth = min(min_depth, d);
                max_depth = max(max_depth, d);
            }
        }
    }
    if (max_depth <= 0.0f)
    {
        tile_light_assignment.Store4(tile_idx * 16 + 0, uint4(4294967295u, 4294967295u, 4294967295u, 4294967295u));
        return;
    }
    float3 avg_normal = 0.0f.xxx;
    uint valid_count = 0u;
    for (uint dy_1 = 0u; dy_1 < tile_size; dy_1 += 2u)
    {
        for (uint dx_1 = 0u; dx_1 < tile_size; dx_1 += 2u)
        {
            uint2 texel_1 = tile_origin + uint2(dx_1, dy_1);
            bool _240 = texel_1.x >= lcp_params.sc_params.atlas_size;
            bool _249;
            if (!_240)
            {
                _249 = texel_1.y >= lcp_params.sc_params.atlas_size;
            }
            else
            {
                _249 = _240;
            }
            if (_249)
            {
                continue;
            }
            float d_1 = depth_atlas[int2(texel_1)].xxxx.x;
            if (d_1 > 0.0f)
            {
                float2 enc = normal_atlas[int2(texel_1)].xyyy.xy;
                float2 param = enc;
                float3 _274 = octDecode(param);
                avg_normal += _274;
                valid_count++;
            }
        }
    }
    uint4 assigned = uint4(4294967295u, 4294967295u, 4294967295u, 4294967295u);
    uint assign_count = 0u;
    if (valid_count > 0u)
    {
        avg_normal = normalize(avg_normal);
        for (uint li = 0u; (li < lcp_params.light_count) && (assign_count < 4u); li++)
        {
            bool visible = false;
            if (uint(asfloat(lights.Load(li * 48 + 44))) == 1u)
            {
                visible = true;
            }
            else
            {
                float light_range = asfloat(lights.Load(li * 48 + 12));
                if (max_depth < light_range)
                {
                    visible = true;
                }
            }
            if (visible)
            {
                switch (assign_count)
                {
                    case 0u:
                    {
                        assigned.x = li;
                        break;
                    }
                    case 1u:
                    {
                        assigned.y = li;
                        break;
                    }
                    case 2u:
                    {
                        assigned.z = li;
                        break;
                    }
                    case 3u:
                    {
                        assigned.w = li;
                        break;
                    }
                }
                assign_count++;
            }
        }
    }
    tile_light_assignment.Store4(tile_idx * 16 + 0, assigned);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
