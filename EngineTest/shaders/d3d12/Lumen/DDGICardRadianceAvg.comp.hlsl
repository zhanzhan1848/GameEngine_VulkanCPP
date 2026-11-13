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

struct SurfaceCacheCard
{
    float4 center;
    float4 extent;
    uint axis_direction;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint mesh_instance_id;
    uint static_mesh_slot;
    uint flags;
    uint page_card;
};

static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer ParamBlock : register(b1, space0)
{
    SurfaceCacheParams pb_params : packoffset(c0);
};

ByteAddressBuffer _48 : register(t2, space0);
RWByteAddressBuffer _90 : register(u3, space0);
Texture2D<float4> lighting_atlas : register(t0, space0);
Texture2D<float4> albedo_atlas : register(t4, space0);
Texture2D<float4> depth_atlas : register(t5, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

void comp_main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint cardIndex = gid / 16u;
    uint blockIndex = gid % 16u;
    if (cardIndex >= pb_params.max_cards)
    {
        return;
    }
    SurfaceCacheCard _52;
    _52.center = asfloat(_48.Load4(cardIndex * 64 + 0));
    _52.extent = asfloat(_48.Load4(cardIndex * 64 + 16));
    _52.axis_direction = _48.Load(cardIndex * 64 + 32);
    _52.resolution = _48.Load(cardIndex * 64 + 36);
    _52.atlas_offset_x = _48.Load(cardIndex * 64 + 40);
    _52.atlas_offset_y = _48.Load(cardIndex * 64 + 44);
    _52.mesh_instance_id = _48.Load(cardIndex * 64 + 48);
    _52.static_mesh_slot = _48.Load(cardIndex * 64 + 52);
    _52.flags = _48.Load(cardIndex * 64 + 56);
    _52.page_card = _48.Load(cardIndex * 64 + 60);
    SurfaceCacheCard card;
    card.center = _52.center;
    card.extent = _52.extent;
    card.axis_direction = _52.axis_direction;
    card.resolution = _52.resolution;
    card.atlas_offset_x = _52.atlas_offset_x;
    card.atlas_offset_y = _52.atlas_offset_y;
    card.mesh_instance_id = _52.mesh_instance_id;
    card.static_mesh_slot = _52.static_mesh_slot;
    card.flags = _52.flags;
    card.page_card = _52.page_card;
    if (card.resolution == 0u)
    {
        _90.Store4(gid * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    uint blockX = blockIndex % 4u;
    uint blockY = blockIndex / 4u;
    float res = float(card.resolution);
    float3 sum = 0.0f.xxx;
    float valid = 0.0f;
    bool _253;
    for (uint j = 0u; j < 2u; j++)
    {
        for (uint i = 0u; i < 2u; i++)
        {
            int2 uv = int2(int(float(card.atlas_offset_x) + ((res * (float((blockX * 2u) + i) + 0.5f)) / 8.0f)), int(float(card.atlas_offset_y) + ((res * (float((blockY * 2u) + j) + 0.5f)) / 8.0f)));
            float3 r = lighting_atlas.Load(int3(uv, 0)).xyz;
            bool legacySurface = (pb_params._pad & 1u) != 0u;
            float4 albedo = albedo_atlas.Load(int3(uv, 0));
            float depth = depth_atlas.Load(int3(uv, 0)).x;
            uint _payload = uint(round(clamp(albedo.w, 0.0f, 1.0f) * 255.0f));
            bool _206 = (_payload & 128u) != 0u;
            bool _212;
            if (_206)
            {
                _212 = !isnan(depth);
            }
            else
            {
                _212 = _206;
            }
            bool _218;
            if (_212)
            {
                _218 = !isinf(depth);
            }
            else
            {
                _218 = _212;
            }
            bool _221 = _218 && (depth >= 0.0f);
            bool _229;
            if (_221)
            {
                _229 = !any(isnan(albedo));
            }
            else
            {
                _229 = _221;
            }
            bool _236;
            if (_229)
            {
                _236 = !any(isinf(albedo));
            }
            else
            {
                _236 = _229;
            }
            bool _244;
            if (_236)
            {
                _244 = !any(isnan(r));
            }
            else
            {
                _244 = _236;
            }
            bool _251;
            if (_244)
            {
                _251 = !any(isinf(r));
            }
            else
            {
                _251 = _244;
            }
            bool measuredSurface = _251;
            if (legacySurface)
            {
                _253 = dot(r, r) > 9.9999999747524270787835121154785e-07f;
            }
            else
            {
                _253 = measuredSurface;
            }
            if (_253)
            {
                sum += r;
                valid += 1.0f;
            }
        }
    }
    if (valid > 0.0f)
    {
        _90.Store4(gid * 16 + 0, asuint(float4(sum / valid.xxx, valid / 4.0f)));
    }
    else
    {
        _90.Store4(gid * 16 + 0, asuint(0.0f.xxxx));
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
