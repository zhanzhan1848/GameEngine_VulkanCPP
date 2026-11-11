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

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

cbuffer ParamBlock : register(b2, space0)
{
    SurfaceCacheParams pb_params : packoffset(c0);
};

RWTexture2D<float> depth_in : register(u0, space0);
RWTexture2D<float> depth_out : register(u1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint2 gid = gl_GlobalInvocationID.xy;
    bool _30 = gid.x >= pb_params.atlas_size;
    bool _40;
    if (!_30)
    {
        _40 = gid.y >= pb_params.atlas_size;
    }
    else
    {
        _40 = _30;
    }
    if (_40)
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
            bool _108 = nb.x < 0;
            bool _115;
            if (!_108)
            {
                _115 = nb.y < 0;
            }
            else
            {
                _115 = _108;
            }
            bool _125;
            if (!_115)
            {
                _125 = nb.x >= int(pb_params.atlas_size);
            }
            else
            {
                _125 = _115;
            }
            bool _135;
            if (!_125)
            {
                _135 = nb.y >= int(pb_params.atlas_size);
            }
            else
            {
                _135 = _125;
            }
            if (_135)
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

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
