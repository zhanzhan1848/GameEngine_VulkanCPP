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

static const uint3 gl_WorkGroupSize = uint3(256u, 1u, 1u);

cbuffer ShadowParamsBlock : register(b5, space0)
{
    float4 sp_sdf_origins[3] : packoffset(c0);
    float4 sp_sdf_extents[3] : packoffset(c3);
    float4 sp_sdf_resolutions_count : packoffset(c6);
    float4 sp_light_direction : packoffset(c7);
    float4 sp_shadow_params : packoffset(c8);
    float4 sp_counts : packoffset(c9);
    float4 sp_shadow_matrices[4][4] : packoffset(c10);
    float4 sp_shadow_meta : packoffset(c26);
};

ByteAddressBuffer card_dispatch : register(t6, space0);
RWByteAddressBuffer vis : register(u8, space0);
ByteAddressBuffer cards : register(t7, space0);
Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);
Texture2D<float4> shadow_depth_0 : register(t9, space0);
Texture2D<float4> shadow_depth_1 : register(t10, space0);
Texture2D<float4> shadow_depth_2 : register(t11, space0);
Texture2D<float4> shadow_depth_3 : register(t12, space0);
RWTexture2D<float> depth_atlas : register(u3, space0);
RWTexture2D<float2> normal_atlas : register(u4, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

float3 octDecode(inout float2 f)
{
    f = (f * 2.0f) - 1.0f.xx;
    float3 n = float3(f.x, f.y, (1.0f - abs(f.x)) - abs(f.y));
    float t = clamp(-n.z, 0.0f, 1.0f);
    float3 _233 = n;
    float3 _237 = n;
    float2 _239 = _237.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _233.xy));
    n.x = _239.x;
    n.y = _239.y;
    return normalize(n);
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card)
{
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5f) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5f) / float(card.resolution);
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float3 local_pos;
    if (axis == 0u)
    {
        local_pos.x = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
        local_pos.y = ((u - 0.5f) * card.extent.y) * 2.0f;
        local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
    }
    else
    {
        if (axis == 1u)
        {
            local_pos.y = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.z = ((v - 0.5f) * card.extent.z) * 2.0f;
        }
        else
        {
            local_pos.z = ((dir > 0u) ? 1.0f : (-1.0f)) * depth;
            local_pos.x = ((u - 0.5f) * card.extent.x) * 2.0f;
            local_pos.y = ((v - 0.5f) * card.extent.y) * 2.0f;
        }
    }
    return local_pos + card.center.xyz;
}

float scPcf4(Texture2D<float4> depthMap, float2 uv, float ndcZ)
{
    uint _534_dummy_parameter;
    int2 dims = int2(spvTextureSize(depthMap, uint(0), _534_dummy_parameter));
    float2 q = (clamp(uv, 0.0f.xx, 1.0f.xx) * float2(dims)) - 0.5f.xx;
    int2 t0 = int2(floor(q));
    float2 f = frac(q);
    int2 t1 = min((t0 + int2(1, 1)), (dims - int2(1, 1)));
    t0 = max(t0, int2(0, 0));
    float lit = 0.0f;
    lit += (((ndcZ - 0.00150000001303851604461669921875f) > depthMap.Load(int3(t0, 0)).x) ? 0.0f : 1.0f);
    lit += (((ndcZ - 0.00150000001303851604461669921875f) > depthMap.Load(int3(int2(t1.x, t0.y), 0)).x) ? 0.0f : 1.0f);
    lit += (((ndcZ - 0.00150000001303851604461669921875f) > depthMap.Load(int3(int2(t0.x, t1.y), 0)).x) ? 0.0f : 1.0f);
    lit += (((ndcZ - 0.00150000001303851604461669921875f) > depthMap.Load(int3(t1, 0)).x) ? 0.0f : 1.0f);
    return lit * 0.25f;
}

float scShadowMapVisibility(float3 world, float3 normal, float footprint)
{
    uint cascadeCount = uint(sp_shadow_meta.x + 0.5f);
    float3 p = world + (normal * (footprint * sp_shadow_meta.y));
    for (uint c = 0u; (c < cascadeCount) && (c < 4u); c++)
    {
        float4 col0 = sp_shadow_matrices[c][0];
        float4 col1 = sp_shadow_matrices[c][1];
        float4 col2 = sp_shadow_matrices[c][2];
        float4 col3 = sp_shadow_matrices[c][3];
        float4 clip = (((col0 * p.x) + (col1 * p.y)) + (col2 * p.z)) + col3;
        if (clip.w <= 9.9999999747524270787835121154785e-07f)
        {
            continue;
        }
        float3 ndc = clip.xyz / clip.w.xxx;
        bool _693 = ndc.x < (-1.0f);
        bool _700;
        if (!_693)
        {
            _700 = ndc.x > 1.0f;
        }
        else
        {
            _700 = _693;
        }
        bool _707;
        if (!_700)
        {
            _707 = ndc.y < (-1.0f);
        }
        else
        {
            _707 = _700;
        }
        bool _714;
        if (!_707)
        {
            _714 = ndc.y > 1.0f;
        }
        else
        {
            _714 = _707;
        }
        bool _721;
        if (!_714)
        {
            _721 = ndc.z < 0.0f;
        }
        else
        {
            _721 = _714;
        }
        bool _728;
        if (!_721)
        {
            _728 = ndc.z > 1.0f;
        }
        else
        {
            _728 = _721;
        }
        if (_728)
        {
            continue;
        }
        float2 uv = float2((ndc.x * 0.5f) + 0.5f, 0.5f - (ndc.y * 0.5f));
        if (c == 0u)
        {
            float2 param = uv;
            float param_1 = ndc.z;
            return scPcf4(shadow_depth_0, param, param_1);
        }
        if (c == 1u)
        {
            float2 param_2 = uv;
            float param_3 = ndc.z;
            return scPcf4(shadow_depth_1, param_2, param_3);
        }
        if (c == 2u)
        {
            float2 param_4 = uv;
            float param_5 = ndc.z;
            return scPcf4(shadow_depth_2, param_4, param_5);
        }
        float2 param_6 = uv;
        float param_7 = ndc.z;
        return scPcf4(shadow_depth_3, param_6, param_7);
    }
    return -1.0f;
}

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    bool _252 = all(bool3(pos.x >= origin.x, pos.y >= origin.y, pos.z >= origin.z));
    bool _261;
    if (_252)
    {
        float3 _258 = origin + extent;
        _261 = all(bool3(pos.x < _258.x, pos.y < _258.y, pos.z < _258.z));
    }
    else
    {
        _261 = _252;
    }
    return _261;
}

float finestVoxelSize()
{
    uint cc = uint(sp_sdf_resolutions_count.w);
    if (cc == 0u)
    {
        return 1.0f;
    }
    float extent = sp_sdf_extents[0].x;
    float res = sp_sdf_resolutions_count.x;
    return max(extent / max(res, 1.0f), 0.00999999977648258209228515625f);
}

float voxelAt(float3 pos)
{
    uint cc = uint(sp_sdf_resolutions_count.w);
    for (uint c = 0u; (c < cc) && (c < 3u); c++)
    {
        float3 origin = sp_sdf_origins[c].xyz;
        float3 param = pos;
        float3 param_1 = origin;
        float3 param_2 = sp_sdf_extents[c].xyz;
        if (isInsideCascade(param, param_1, param_2))
        {
            float res = sp_sdf_resolutions_count[c];
            return max(sp_sdf_extents[c].x / max(res, 1.0f), 0.00999999977648258209228515625f);
        }
    }
    return finestVoxelSize();
}

float sampleSDF(Texture3D<float4> tex, float3 pos, float3 origin, float3 extent, uint res)
{
    float3 uvw = (pos - origin) / extent;
    bool _272 = uvw.x < 0.0f;
    bool _279;
    if (!_272)
    {
        _279 = uvw.x > 1.0f;
    }
    else
    {
        _279 = _272;
    }
    bool _286;
    if (!_279)
    {
        _286 = uvw.y < 0.0f;
    }
    else
    {
        _286 = _279;
    }
    bool _293;
    if (!_286)
    {
        _293 = uvw.y > 1.0f;
    }
    else
    {
        _293 = _286;
    }
    bool _300;
    if (!_293)
    {
        _300 = uvw.z < 0.0f;
    }
    else
    {
        _300 = _293;
    }
    bool _307;
    if (!_300)
    {
        _307 = uvw.z > 1.0f;
    }
    else
    {
        _307 = _300;
    }
    if (_307)
    {
        return 10000000000.0f;
    }
    int r = int(res);
    int3 t = clamp(int3(uvw * float(r)), int3(0, 0, 0), (r - 1).xxx);
    return tex.Load(int4(t, 0)).x;
}

float sampleBestSDF(float3 pos)
{
    uint cc = uint(sp_sdf_resolutions_count.w);
    bool _350 = cc > 0u;
    bool _365;
    if (_350)
    {
        float3 param = pos;
        float3 param_1 = sp_sdf_origins[0].xyz;
        float3 param_2 = sp_sdf_extents[0].xyz;
        _365 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _365 = _350;
    }
    if (_365)
    {
        float3 param_3 = pos;
        float3 param_4 = sp_sdf_origins[0].xyz;
        float3 param_5 = sp_sdf_extents[0].xyz;
        uint param_6 = uint(sp_sdf_resolutions_count.x);
        return sampleSDF(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    bool _386 = cc > 1u;
    bool _400;
    if (_386)
    {
        float3 param_7 = pos;
        float3 param_8 = sp_sdf_origins[1].xyz;
        float3 param_9 = sp_sdf_extents[1].xyz;
        _400 = isInsideCascade(param_7, param_8, param_9);
    }
    else
    {
        _400 = _386;
    }
    if (_400)
    {
        float3 param_10 = pos;
        float3 param_11 = sp_sdf_origins[1].xyz;
        float3 param_12 = sp_sdf_extents[1].xyz;
        uint param_13 = uint(sp_sdf_resolutions_count.y);
        return sampleSDF(sdf_cascade_1, param_10, param_11, param_12, param_13);
    }
    bool _421 = cc > 2u;
    bool _435;
    if (_421)
    {
        float3 param_14 = pos;
        float3 param_15 = sp_sdf_origins[2].xyz;
        float3 param_16 = sp_sdf_extents[2].xyz;
        _435 = isInsideCascade(param_14, param_15, param_16);
    }
    else
    {
        _435 = _421;
    }
    if (_435)
    {
        float3 param_17 = pos;
        float3 param_18 = sp_sdf_origins[2].xyz;
        float3 param_19 = sp_sdf_extents[2].xyz;
        uint param_20 = uint(sp_sdf_resolutions_count.z);
        return sampleSDF(sdf_cascade_2, param_17, param_18, param_19, param_20);
    }
    return 10000000000.0f;
}

float scEvaluateShadowPoint(float3 world, float3 normal, float footprint)
{
    if (sp_shadow_meta.x > 0.5f)
    {
        float3 param = world;
        float3 param_1 = normal;
        float param_2 = footprint;
        float v = scShadowMapVisibility(param, param_1, param_2);
        if (v >= 0.0f)
        {
            return v;
        }
    }
    float3 param_3 = world;
    float voxel = voxelAt(param_3);
    float3 origin = world + (normal * (voxel * sp_shadow_params.y));
    float3 param_4 = origin;
    float d0 = sampleBestSDF(param_4);
    float3 L = sp_light_direction.xyz;
    float maxDist = sp_shadow_params.x;
    float t = voxel;
    float hitThreshold = min(voxel * 0.5f, max(d0 * 0.5f, 0.0f));
    float shadow = 1.0f;
    float _880;
    for (int _step = 0; _step < 16; _step++)
    {
        float3 pos = origin + (L * t);
        float3 param_5 = pos;
        float d = sampleBestSDF(param_5);
        if (d > 1000000000.0f)
        {
            break;
        }
        if (d < hitThreshold)
        {
            shadow = 0.0f;
            break;
        }
        if (d > (hitThreshold * 4.0f))
        {
            _880 = d * 1.2000000476837158203125f;
        }
        else
        {
            _880 = max(d, voxel * 0.25f);
        }
        float advance = _880;
        t += advance;
        if (t >= maxDist)
        {
            break;
        }
    }
    return shadow;
}

void comp_main()
{
    uint global_tid = gl_GlobalInvocationID.x;
    if (global_tid >= uint(sp_counts.x))
    {
        return;
    }
    uint lo = 0u;
    uint hi = uint(sp_counts.y);
    while (lo < hi)
    {
        uint mid = (lo + hi) >> uint(1);
        if ((card_dispatch.Load(mid * 32 + 0) + card_dispatch.Load(mid * 32 + 4)) <= global_tid)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo >= uint(sp_counts.y))
    {
        return;
    }
    uint local_texel = global_tid - card_dispatch.Load(lo * 32 + 0);
    uint res = card_dispatch.Load(lo * 32 + 8);
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_uv = uint2(card_dispatch.Load(lo * 32 + 12) + local_x, card_dispatch.Load(lo * 32 + 16) + local_y);
    uint card_idx = card_dispatch.Load(lo * 32 + 20);
    uint atlas_size = uint(sp_shadow_params.z);
    uint vis_index = atlas_uv.x + (atlas_size * atlas_uv.y);
    float depth = depth_atlas[int2(atlas_uv)].xxxx.x;
    if (depth <= 0.0f)
    {
        vis.Store(vis_index * 4 + 0, asuint(1.0f));
        return;
    }
    float2 enc_n = normal_atlas[int2(atlas_uv)].xyyy.xy;
    float2 param = enc_n;
    float3 _1046 = octDecode(param);
    float3 normal = _1046;
    uint2 param_1 = atlas_uv;
    float param_2 = depth;
    SurfaceCacheCard _1061;
    _1061.center = asfloat(cards.Load4(card_idx * 64 + 0));
    _1061.extent = asfloat(cards.Load4(card_idx * 64 + 16));
    _1061.axis_direction = cards.Load(card_idx * 64 + 32);
    _1061.resolution = cards.Load(card_idx * 64 + 36);
    _1061.atlas_offset_x = cards.Load(card_idx * 64 + 40);
    _1061.atlas_offset_y = cards.Load(card_idx * 64 + 44);
    _1061.mesh_instance_id = cards.Load(card_idx * 64 + 48);
    _1061.static_mesh_slot = cards.Load(card_idx * 64 + 52);
    _1061.flags = cards.Load(card_idx * 64 + 56);
    _1061.page_card = cards.Load(card_idx * 64 + 60);
    SurfaceCacheCard param_3;
    param_3.center = _1061.center;
    param_3.extent = _1061.extent;
    param_3.axis_direction = _1061.axis_direction;
    param_3.resolution = _1061.resolution;
    param_3.atlas_offset_x = _1061.atlas_offset_x;
    param_3.atlas_offset_y = _1061.atlas_offset_y;
    param_3.mesh_instance_id = _1061.mesh_instance_id;
    param_3.static_mesh_slot = _1061.static_mesh_slot;
    param_3.flags = _1061.flags;
    param_3.page_card = _1061.page_card;
    float3 world = cardTexelToWorld(param_1, param_2, param_3);
    float texelRes = float(max(cards.Load(card_idx * 64 + 36), 1u));
    float footprint = length(asfloat(cards.Load4(card_idx * 64 + 16)).xyz) / texelRes;
    float3 param_4 = world;
    float3 param_5 = normal;
    float param_6 = footprint;
    vis.Store(vis_index * 4 + 0, asuint(scEvaluateShadowPoint(param_4, param_5, param_6)));
}

[numthreads(256, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
