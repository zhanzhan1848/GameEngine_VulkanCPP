// Vulkan reference: Lumen/SurfaceCacheLightEval.comp (surface_cache_light_eval).
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

struct LightInfo
{
    float4 position;
    float4 color;
    float4 direction;
};

ByteAddressBuffer ddgiDepth : register(t13, space0);
ByteAddressBuffer ddgiIrr : register(t10, space0);
cbuffer FlattenedLightingParamsBlock : register(b4, space0)
{
    FlattenedLightingParams flp_params : packoffset(c0);
};

ByteAddressBuffer ddgiOpenSky : register(t14, space0);
ByteAddressBuffer card_dispatch : register(t7, space0);
ByteAddressBuffer pageTable : register(t15, space0);
ByteAddressBuffer cards : register(t5, space0);
ByteAddressBuffer shadow_vis : register(t9, space0);
ByteAddressBuffer lights : register(t6, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u0, space0);
RWTexture2D<float> depth_atlas : register(u11, space0);
RWTexture2D<float4> lighting_out : register(u3, space0);
RWTexture2D<float4> nodirect_out : register(u16, space0);
RWTexture2D<float2> normal_atlas : register(u1, space0);
RWTexture2D<float4> emissive_atlas : register(u2, space0);
RWTexture2D<float4> prev_lighting_atlas : register(u8, space0);
RWTexture2D<float4> indirect_out : register(u12, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 octDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    // Inverse of the replace-fold in octEncode: n.xy >= 0 subtracts t, else adds.
    n.xy += lerp((float2)(t), (float2)(-t), step((float2)(0.0), n.xy));
    return normalize(n);
}

float3 cardTexelToWorld(uint2 atlas_uv, float depth, SurfaceCacheCard card) {    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    float3 local_pos;
    if (axis == 0u) {
        local_pos.x = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (axis == 1u) {
        local_pos.y = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else {
        local_pos.z = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center.xyz;
}

float3 evalProbeIrradianceSH9(float3 sh[9], float3 n)
{
    float x = n.x;
    float y = n.y;
    float z = n.z;
    float xy = x * y;
    float yz = y * z;
    float zx = z * x;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    return ((((((((sh[0] * 0.28209478f) + (sh[1] * ((-0.32573503f) * y))) + (sh[2] * (0.32573503f * z))) + (sh[3] * ((-0.32573503f) * x))) + (sh[4] * (0.27313712f * xy))) + (sh[5] * ((-0.27313712f) * yz))) + (sh[6] * (0.07884789f * ((3.0f * zz) - 1.0f)))) + (sh[7] * ((-0.27313712f) * zx))) + (sh[8] * (0.13656855f * (xx - yy)));
}

float3 gatherProbeSH(uint pidx, float3 N)
{
    float3 sh[9];
    for (int i = 0; i < 9; i++)
    {
        sh[i] = asfloat(ddgiIrr.Load4(((pidx * 9u) + uint(i)) * 16)).xyz;
    }
    float3 param[9] = sh;
    float3 param_1 = N;
    float3 irr = evalProbeIrradianceSH9(param, param_1);
    return max(irr, 0.0f.xxx);
}

float2 scGatherOctEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 u = d.xy / l1norm;
    float2 s = float2(u.x >= 0.0 ? 1.0 : -1.0, u.y >= 0.0 ? 1.0 : -1.0);
    if (d.z < 0.0) u = (1.0 - abs(float2(u.y, u.x))) * s;
    return u * 0.5 + 0.5;
}

float2 scSampleProbeDepthMV(uint probeIdx, float3 dirToSurface)
{
    float3 param = dirToSurface;
    float2 uv = scGatherOctEncode(param);
    uint2 texel = uint2(clamp(uint(uv.x * 8.0f), 0u, 7u), clamp(uint(uv.y * 8.0f), 0u, 7u));
    uint octIdx = (texel.y * 8u) + texel.x;
    uint base = probeIdx * 128u;
    return float2(asfloat(ddgiDepth.Load((base + octIdx) * 4)), asfloat(ddgiDepth.Load(((base + 64u) + octIdx) * 4)));
}

float scVisibilityWeight(float distToProbe, float mean, float variance) {
    float dist = max(distToProbe - mean, 0.0);
    if (variance < 0.0001) return 1.0;
    float cheb = variance / (variance + dist * dist);
    return clamp(max(cheb * 2.0, 0.2), 0.0, 1.0);
}

float3 gatherProbeIrradiance(float3 worldPos, float3 N, out bool inVolume)
{
    inVolume = false;
    if (flp_params.ddgi_counts_valid.w == 0u)
    {
        return 0.0f.xxx;
    }
    float3 org = flp_params.ddgi_origin_spacing.xyz;
    float spacing = max(flp_params.ddgi_origin_spacing.w, 0.01f);
    int3 counts = int3(flp_params.ddgi_counts_valid.xyz);
    float3 gp = (worldPos - org) / spacing.xxx;
    float3 gridMax = float3(counts - int3(1, 1, 1));
    if (((any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z))) || (any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z)))))
    {
        return 0.0f.xxx;
    }
    inVolume = true;
    int3 b0 = clamp(int3(floor(gp)), int3(0, 0, 0), counts - int3(1, 1, 1));
    int3 b1 = min((b0 + int3(1, 1, 1)), (counts - int3(1, 1, 1)));
    float3 fr = clamp(frac(gp), 0.0f.xxx, 1.0f.xxx);
    float pixOpen = 0.0f;
    float pixOpenW = 0.0f;
    if (flp_params.radiosity_gate.w != 0u)
    {
        for (int z = 0; z < 2; z++)
        {
            for (int y = 0; y < 2; y++)
            {
                for (int x = 0; x < 2; x++)
                {
                    float tw = (lerp(1.0f - fr.x, fr.x, float(x)) * lerp(1.0f - fr.y, fr.y, float(y))) * lerp(1.0f - fr.z, fr.z, float(z));
                    if (tw <= 0.0f)
                    {
                        continue;
                    }
                    int3 pc = int3(lerp(float3(b0), float3(b1), float3(int3(x, y, z))));
                    uint pidx = uint(pc.x) + (uint(counts.x) * (uint(pc.y) + (uint(counts.y) * uint(pc.z))));
                    pixOpen += (asfloat(ddgiOpenSky.Load((pidx * 2u) * 4)) * tw);
                    pixOpenW += tw;
                }
            }
        }
        if (pixOpenW > 0.0f)
        {
            pixOpen /= pixOpenW;
        }
    }
    float3 accVis = 0.0f.xxx;
    float3 accLow = 0.0f.xxx;
    float visW = 0.0f;
    float lowW = 0.0f;
    float _921;
    for (int z_1 = 0; z_1 < 2; z_1++)
    {
        for (int y_1 = 0; y_1 < 2; y_1++)
        {
            for (int x_1 = 0; x_1 < 2; x_1++)
            {
                float tw_1 = (lerp(1.0f - fr.x, fr.x, float(x_1)) * lerp(1.0f - fr.y, fr.y, float(y_1))) * lerp(1.0f - fr.z, fr.z, float(z_1));
                if (tw_1 <= 0.0f)
                {
                    continue;
                }
                int3 pc_1 = int3(lerp(float3(b0), float3(b1), float3(int3(x_1, y_1, z_1))));
                uint pidx_1 = uint(pc_1.x) + (uint(counts.x) * (uint(pc_1.y) + (uint(counts.y) * uint(pc_1.z))));
                uint param = pidx_1;
                float3 param_1 = N;
                float3 irr = gatherProbeSH(param, param_1);
                float3 probePos = org + (float3(pc_1) * spacing);
                float3 biasedPos = worldPos + ((N * spacing) * 0.05f);
                float3 toProbeVec = probePos - biasedPos;
                float distToProbe = length(toProbeVec);
                float3 toProbe = toProbeVec / max(distToProbe, 1e-05f).xxx;
                uint param_2 = pidx_1;
                float3 param_3 = -toProbe;
                float2 depthMV = scSampleProbeDepthMV(param_2, param_3);
                float insidePenalty = 1.0f;
                float burial = 1.0f;
                if (depthMV.x < (spacing * 0.2f))
                {
                    insidePenalty = lerp(0.2f, 1.0f, smoothstep(0.0f, 0.2f, depthMV.x / spacing));
                    burial = lerp(0.1f, 1.0f, smoothstep(0.0f, 0.2f, depthMV.x / spacing));
                }
                float normalWeight = lerp(0.2f, 1.0f, clamp((dot(N, toProbe) + 0.5f) / 0.7f, 0.0f, 1.0f));
                float nwSoft = max(normalWeight, 0.4f);
                float param_4 = distToProbe;
                float param_5 = depthMV.x;
                float param_6 = depthMV.y;
                float vw = max(scVisibilityWeight(param_4, param_5, param_6), 0.38f);
                float atten = 1.0f;
                if (flp_params.radiosity_gate.w != 0u)
                {
                    float pOpen = asfloat(ddgiOpenSky.Load((pidx_1 * 2u) * 4));
                    float skyness = smoothstep(0.15f, 0.45f, pOpen);
                    float enclosed = 1.0f - smoothstep(0.05f, 0.25f, pixOpen);
                    float classify = 1.0f - ((skyness * enclosed) * 0.85f);
                    if (flp_params.opensky_v2 != 0u)
                    {
                        _921 = lerp(1.0f, classify, asfloat(ddgiOpenSky.Load(((pidx_1 * 2u) + 1u) * 4)));
                    }
                    else
                    {
                        _921 = classify;
                    }
                    atten = _921;
                }
                accVis += (((((irr * tw_1) * nwSoft) * vw) * insidePenalty) * atten);
                visW += ((((tw_1 * nwSoft) * vw) * insidePenalty) * atten);
                accLow += (((irr * tw_1) * burial) * atten);
                lowW += ((tw_1 * burial) * atten);
            }
        }
    }
    float3 result = 0.0f.xxx;
    if (visW > 0.0f)
    {
        result += ((accVis / visW.xxx) * 0.45f);
    }
    if (lowW > 0.0f)
    {
        result += ((accLow / lowW.xxx) * 0.55f);
    }
    if ((visW <= 0.0f) && (lowW <= 0.0f))
    {
        return 0.0f.xxx;
    }
    result *= 3.1415927f;
    return min(result, 64.0f.xxx);
}

float scRadiosityWeight(uint bits) {
    return bits != 0u ? asfloat(bits) : 0.25;
}

[numthreads(256, 1, 1)]
void main(StageInput input)
{
    uint global_tid = input.dispatchThreadID.x;
    if (global_tid >= flp_params.total_texels)
    {
        return;
    }
    uint lo = 0u;
    uint hi = flp_params.card_count;
    while (lo < hi)
    {
        uint mid = (lo + hi) >> 1u;
        if ((card_dispatch.Load(mid * 32) + card_dispatch.Load(mid * 32 + 4)) <= global_tid)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo >= flp_params.card_count)
    {
        return;
    }
    uint local_texel = global_tid - card_dispatch.Load(lo * 32);
    uint res = card_dispatch.Load(lo * 32 + 8);
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_origin = uint2(card_dispatch.Load(lo * 32 + 12), card_dispatch.Load(lo * 32 + 16));
    if (flp_params.page_table_enabled != 0u)
    {
        uint ptEntry = pageTable.Load(card_dispatch.Load(lo * 32 + 20) * 4);
        uint ptPage = max(flp_params.sc_params.page_size, 1u);
        atlas_origin = uint2((ptEntry >> 16u) & 65535u, ptEntry & 65535u) * ptPage.xx;
    }
    uint2 atlas_uv = atlas_origin + uint2(local_x, local_y);
    uint card_idx = card_dispatch.Load(lo * 32 + 20);
    float4 albedo = albedo_atlas[int2(atlas_uv)];
    bool legacySurface = (flp_params.sc_params._pad & 1u) != 0u;
    float depth = depth_atlas[int2(atlas_uv)].xxxx.x;
    uint transportBits = uint(round(clamp(albedo.w, 0.0f, 1.0f) * 255.0f));
    if (legacySurface)
    {
        if (albedo.w < 0.01f)
        {
            return;
        }
    }
    else
    {
        bool _1212 = (((transportBits & 128u) == 0u) || (isnan(depth))) || (isinf(depth));
        if ((((_1212 || (depth < 0.0f)) || (any(isnan(albedo)))) || (any(isinf(albedo)))))
        {
            lighting_out[int2(atlas_uv)] = 0.0f.xxxx;
            nodirect_out[int2(atlas_uv)] = 0.0f.xxxx;
            return;
        }
    }
    float transmission = float((transportBits >> 3u) & 15u) / 15.0f;
    float thickness = float(transportBits & 7u) / 7.0f;
    float backTransmittance = transmission * exp((-2.0f) * thickness);
    float2 enc_n = normal_atlas[int2(atlas_uv)].xyyy.xy;
    float2 param = enc_n;
    float3 _1278 = octDecode(param);
    float3 normal = _1278;
    float3 emissive = emissive_atlas[int2(atlas_uv)].xyz;
    emissive = min(emissive, 8.0f.xxx);
    uint2 param_1 = atlas_uv;
    float param_2 = max(depth, 0.0f);
    SurfaceCacheCard _1304;
    _1304.center = asfloat(cards.Load4(card_idx * 64));
    _1304.extent = asfloat(cards.Load4(card_idx * 64 + 16));
    _1304.axis_direction = cards.Load(card_idx * 64 + 32);
    _1304.resolution = cards.Load(card_idx * 64 + 36);
    _1304.atlas_offset_x = cards.Load(card_idx * 64 + 40);
    _1304.atlas_offset_y = cards.Load(card_idx * 64 + 44);
    _1304.mesh_instance_id = cards.Load(card_idx * 64 + 48);
    _1304.static_mesh_slot = cards.Load(card_idx * 64 + 52);
    _1304.flags = cards.Load(card_idx * 64 + 56);
    _1304.page_card = cards.Load(card_idx * 64 + 60);
    SurfaceCacheCard param_3;
    param_3.center = _1304.center;
    param_3.extent = _1304.extent;
    param_3.axis_direction = _1304.axis_direction;
    param_3.resolution = _1304.resolution;
    param_3.atlas_offset_x = _1304.atlas_offset_x;
    param_3.atlas_offset_y = _1304.atlas_offset_y;
    param_3.mesh_instance_id = _1304.mesh_instance_id;
    param_3.static_mesh_slot = _1304.static_mesh_slot;
    param_3.flags = _1304.flags;
    param_3.page_card = _1304.page_card;
    float3 world_pos = cardTexelToWorld(param_1, param_2, param_3);
    float3 direct_light = 0.0f.xxx;
    uint lightCount = min(flp_params.light_count, 256u);
    uint atlas_size = flp_params.sc_params.atlas_size;
    float dir_visibility = asfloat(shadow_vis.Load((atlas_uv.x + (atlas_size * atlas_uv.y)) * 4));
    float3 L;
    float attenuation;
    for (uint li = 0u; li < lightCount; li++)
    {
        uint lightType = uint(asfloat(lights.Load(li * 48 + 44)));
        if (lightType == 1u)
        {
            L = -normalize(asfloat(lights.Load4(li * 48 + 32)).xyz);
            attenuation = dir_visibility;
        }
        else
        {
            float3 to_light = asfloat(lights.Load4(li * 48)).xyz - world_pos;
            float dist = length(to_light);
            float radius = asfloat(lights.Load(li * 48 + 12));
            if (dist > radius)
            {
                continue;
            }
            L = to_light / max(dist, 0.001f).xxx;
            float r2 = radius * radius;
            attenuation = max(1.0f - ((dist * dist) / r2), 0.0f);
        }
        float signedNdotL = dot(normal, L);
        float frontTerm = max(signedNdotL, 0.0f);
        float backTerm = max(-signedNdotL, 0.0f) * backTransmittance;
        direct_light += ((asfloat(lights.Load4(li * 48 + 16)).xyz * (frontTerm + backTerm)) * attenuation);
    }
    float3 prev_light = prev_lighting_atlas[int2(atlas_uv)].xyz;
    if (((any(isnan(prev_light))) || (any(isinf(prev_light)))))
    {
        prev_light = 0.0f.xxx;
    }
    prev_light = clamp(prev_light, 0.0f.xxx, 256.0f.xxx);
    bool probeInVolume = false;
    float3 param_4 = world_pos;
    float3 param_5 = normal;
    bool param_6;
    float3 _1476 = gatherProbeIrradiance(param_4, param_5, param_6);
    probeInVolume = param_6;
    float3 E_indirect = _1476;
    bool probeFed = flp_params.ddgi_counts_valid.w != 0u;
    E_indirect *= flp_params.probe_feed_bootstrap;
    float3 tileRadiance = 0.0f.xxx;
    if (flp_params.radiosity_gate.x != 0u)
    {
        float4 tileSample = indirect_out[int2(atlas_uv)];
        if (tileSample.w > 0.0f)
        {
            tileRadiance = tileSample.xyz;
        }
    }
    float3 _1517;
    if (flp_params.radiosity_solo != 0u)
    {
        _1517 = tileRadiance * scRadiosityWeight(flp_params.radiosity_gate.y);
    }
    else
    {
        float3 _1530;
        if (probeFed && probeInVolume)
        {
            _1530 = tileRadiance * scRadiosityWeight(flp_params.radiosity_gate.y);
        }
        else
        {
            _1530 = tileRadiance + ((prev_light * albedo.xyz) * 0.31830987f);
        }
        _1517 = _1530;
    }
    float3 result = ((((direct_light + E_indirect) * albedo.xyz) * 0.31830987f) + _1517) + emissive;
    result = min(result, 64.0f.xxx);
    lighting_out[int2(atlas_uv)] = float4(result, 1.0f);
    float3 result_nodirect = result - ((direct_light * albedo.xyz) * 0.31830987f);
    result_nodirect = max(result_nodirect, 0.0f.xxx);
    result_nodirect = min(result_nodirect, 64.0f.xxx);
    nodirect_out[int2(atlas_uv)] = float4(result_nodirect, 1.0f);
}
