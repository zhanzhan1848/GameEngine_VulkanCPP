struct SCGridParams
{
    float3 grid_min;
    float3 grid_max;
    uint3 dims;
    float cell_size;
    uint card_count;
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

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

ByteAddressBuffer scgo : register(t8, space0);
ByteAddressBuffer cb : register(t7, space0);
ByteAddressBuffer scgc : register(t9, space0);
cbuffer CaptureParams : register(b6, space0)
{
    row_major float4x4 cp_invViewProjection : packoffset(c0);
    uint cp_card_count : packoffset(c4);
    uint cp_render_width : packoffset(c4.y);
    uint cp_render_height : packoffset(c4.z);
    uint cp_atlas_size : packoffset(c4.w);
};

RWByteAddressBuffer cdm : register(u10, space0);
cbuffer CaptureEmissive : register(b12, space0)
{
    float4 ce_emissive_rgba : packoffset(c0);
};

ByteAddressBuffer ie : register(t13, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u3, space0);
RWTexture2D<float2> normal_atlas : register(u4, space0);
RWTexture2D<float> depth_atlas : register(u5, space0);
RWTexture2D<float4> emissive_atlas : register(u11, space0);
Texture2D<float4> gbuffer_depth : register(t0, space0);
Texture2D<float4> gbuffer_albedo : register(t1, space0);
Texture2D<float4> gbuffer_normal : register(t2, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float2 octEncode(float3 n)
{
    float l1norm = (abs(n.x) + abs(n.y)) + abs(n.z);
    float2 result = n.xy / l1norm.xx;
    if (n.z < 0.0f)
    {
        result = (1.0f.xx - abs(result.yx)) * lerp((-1.0f).xx, 1.0f.xx, step(0.0f.xx, result));
    }
    return (result * 0.5f) + 0.5f.xx;
}

SCGridParams scGridDecode(uint4 h0, uint4 h1, uint4 h2)
{
    SCGridParams gp;
    gp.grid_min = asfloat(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = asfloat(h1.xyz);
    gp.cell_size = asfloat(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

uint scGridCellIndex(SCGridParams gp, float3 p)
{
    float3 g = (p - gp.grid_min) / gp.cell_size.xxx;
    bool _142 = g.x < 0.0f;
    bool _149;
    if (!_142)
    {
        _149 = g.y < 0.0f;
    }
    else
    {
        _149 = _142;
    }
    bool _156;
    if (!_149)
    {
        _156 = g.z < 0.0f;
    }
    else
    {
        _156 = _149;
    }
    if (_156)
    {
        return 4294967295u;
    }
    uint3 c = uint3(g);
    bool _168 = c.x >= gp.dims.x;
    bool _177;
    if (!_168)
    {
        _177 = c.y >= gp.dims.y;
    }
    else
    {
        _177 = _168;
    }
    bool _186;
    if (!_177)
    {
        _186 = c.z >= gp.dims.z;
    }
    else
    {
        _186 = _177;
    }
    if (_186)
    {
        return 4294967295u;
    }
    return c.x + (gp.dims.x * (c.y + (gp.dims.y * c.z)));
}

bool projectToCard(float3 worldPos, SurfaceCacheCard card, inout float2 out_uv, inout float out_depth)
{
    float3 local = worldPos - card.center.xyz;
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float depth;
    float u;
    float v;
    if (axis == 0u)
    {
        depth = local.x * ((dir > 0u) ? 1.0f : (-1.0f));
        u = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
    }
    else
    {
        if (axis == 1u)
        {
            depth = local.y * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.z / (card.extent.z * 2.0f)) + 0.5f;
        }
        else
        {
            depth = local.z * ((dir > 0u) ? 1.0f : (-1.0f));
            u = (local.x / (card.extent.x * 2.0f)) + 0.5f;
            v = (local.y / (card.extent.y * 2.0f)) + 0.5f;
        }
    }
    if (((((depth < 0.0f) || (u < 0.0f)) || (u >= 1.0f)) || (v < 0.0f)) || (v >= 1.0f))
    {
        return false;
    }
    out_uv = float2(float(card.atlas_offset_x) + (u * float(card.resolution)), float(card.atlas_offset_y) + (v * float(card.resolution)));
    out_depth = depth;
    return true;
}

bool surfaceCacheScatter(float3 worldPos, float3 albedo, float2 encN, bool writePhase)
{
    uint4 gh0 = uint4(scgo.Load(0), scgo.Load(4), scgo.Load(8), scgo.Load(12));
    uint4 gh1 = uint4(scgo.Load(16), scgo.Load(20), scgo.Load(24), scgo.Load(28));
    uint4 gh2 = uint4(scgo.Load(32), scgo.Load(36), scgo.Load(40), scgo.Load(44));
    uint4 param = gh0;
    uint4 param_1 = gh1;
    uint4 param_2 = gh2;
    SCGridParams gp = scGridDecode(param, param_1, param_2);
    SCGridParams param_3 = gp;
    float3 param_4 = worldPos;
    uint cell = scGridCellIndex(param_3, param_4);
    if (cell == 4294967295u)
    {
        return false;
    }
    bool wrote = false;
    uint begin = scgo.Load((16u + cell) * 4 + 0);
    uint end = scgo.Load((17u + cell) * 4 + 0);
    SurfaceCacheCard card;
    float2 param_7;
    float param_8;
    for (uint gi = begin; gi < end; gi++)
    {
        SurfaceCacheCard _437;
        _437.center = asfloat(cb.Load4(scgc.Load(gi * 4 + 0) * 64 + 0));
        _437.extent = asfloat(cb.Load4(scgc.Load(gi * 4 + 0) * 64 + 16));
        _437.axis_direction = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 32);
        _437.resolution = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 36);
        _437.atlas_offset_x = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 40);
        _437.atlas_offset_y = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 44);
        _437.mesh_instance_id = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 48);
        _437.static_mesh_slot = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 52);
        _437.flags = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 56);
        _437.page_card = cb.Load(scgc.Load(gi * 4 + 0) * 64 + 60);
        card.center = _437.center;
        card.extent = _437.extent;
        card.axis_direction = _437.axis_direction;
        card.resolution = _437.resolution;
        card.atlas_offset_x = _437.atlas_offset_x;
        card.atlas_offset_y = _437.atlas_offset_y;
        card.mesh_instance_id = _437.mesh_instance_id;
        card.static_mesh_slot = _437.static_mesh_slot;
        card.flags = _437.flags;
        card.page_card = _437.page_card;
        if ((card.flags & 1u) != 0u)
        {
            continue;
        }
        float3 param_5 = worldPos;
        SurfaceCacheCard param_6 = card;
        bool _473 = projectToCard(param_5, param_6, param_7, param_8);
        float2 atlasUV = param_7;
        float axisDepth = param_8;
        if (_473)
        {
            int2 texel = int2(atlasUV);
            uint texelIdx = (uint(texel.y) * cp_atlas_size) + uint(texel.x);
            if (!writePhase)
            {
                uint _511;
                cdm.InterlockedMin(texelIdx * 4 + 0, asuint(axisDepth), _511);
            }
            else
            {
                if (cdm.Load(texelIdx * 4 + 0) == asuint(axisDepth))
                {
                    albedo_atlas[texel] = float4(albedo, 0.501960813999176025390625f);
                    normal_atlas[texel] = float4(encN, 0.0f, 1.0f).xy;
                    depth_atlas[texel] = float4(axisDepth, 0.0f, 0.0f, 1.0f).x;
                    float3 em = ce_emissive_rgba.xyz;
                    bool emOn = ce_emissive_rgba.w > 0.5f;
                    uint iid = card.mesh_instance_id;
                    float4 ieV = asfloat(ie.Load4(min(iid, 1023u) * 16 + 0));
                    if (ieV.w > 0.5f)
                    {
                        em = ieV.xyz;
                        emOn = true;
                    }
                    if (emOn)
                    {
                        emissive_atlas[texel] = float4(em, 1.0f);
                    }
                    wrote = true;
                }
            }
        }
    }
    return wrote;
}

void comp_main()
{
    int2 pixel = int2(gl_GlobalInvocationID.xy);
    bool _611 = pixel.x >= int(cp_render_width);
    bool _621;
    if (!_611)
    {
        _621 = pixel.y >= int(cp_render_height);
    }
    else
    {
        _621 = _611;
    }
    if (_621)
    {
        return;
    }
    float depth = gbuffer_depth.Load(int3(pixel, 0)).x;
    if (depth >= 1.0f)
    {
        return;
    }
    float2 uv = (float2(pixel) + 0.5f.xx) / float2(float(cp_render_width), float(cp_render_height));
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 worldPosH = mul(float4(ndc, depth, 1.0f), cp_invViewProjection);
    float3 worldPos = worldPosH.xyz / worldPosH.w.xxx;
    float3 albedo = gbuffer_albedo.Load(int3(pixel, 0)).xyz;
    float3 n = gbuffer_normal.Load(int3(pixel, 0)).xyz;
    n = normalize((n * 2.0f) - 1.0f.xxx);
    float3 param = n;
    float2 encN = octEncode(param);
    float3 param_1 = worldPos;
    float3 param_2 = albedo;
    float2 param_3 = encN;
    bool param_4 = true;
    bool _706 = surfaceCacheScatter(param_1, param_2, param_3, param_4);
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
