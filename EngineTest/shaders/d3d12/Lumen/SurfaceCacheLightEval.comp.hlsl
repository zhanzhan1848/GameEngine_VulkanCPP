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

static const uint3 gl_WorkGroupSize = uint3(256u, 1u, 1u);

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
    float3 _227 = n;
    float3 _231 = n;
    float2 _233 = _231.xy + lerp(t.xx, (-t).xx, step(0.0f.xx, _227.xy));
    n.x = _233.x;
    n.y = _233.y;
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
    return ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.325735032558441162109375f) * y))) + (sh[2] * (0.325735032558441162109375f * z))) + (sh[3] * ((-0.325735032558441162109375f) * x))) + (sh[4] * (0.2731371223926544189453125f * xy))) + (sh[5] * ((-0.2731371223926544189453125f) * yz))) + (sh[6] * (0.078847892582416534423828125f * ((3.0f * zz) - 1.0f)))) + (sh[7] * ((-0.2731371223926544189453125f) * zx))) + (sh[8] * (0.136568546295166015625f * (xx - yy)));
}

float3 gatherProbeSH(uint pidx, float3 N)
{
    float3 sh[9];
    for (int i = 0; i < 9; i++)
    {
        sh[i] = asfloat(ddgiIrr.Load4(((pidx * 9u) + uint(i)) * 16 + 0)).xyz;
    }
    float3 param[9] = sh;
    float3 param_1 = N;
    float3 irr = evalProbeIrradianceSH9(param, param_1);
    return max(irr, 0.0f.xxx);
}

float2 scGatherOctEncode(float3 d)
{
    float l1norm = (abs(d.x) + abs(d.y)) + abs(d.z);
    float2 u = d.xy / l1norm.xx;
    float2 s = float2((u.x >= 0.0f) ? 1.0f : (-1.0f), (u.y >= 0.0f) ? 1.0f : (-1.0f));
    if (d.z < 0.0f)
    {
        u = (1.0f.xx - abs(float2(u.y, u.x))) * s;
    }
    return (u * 0.5f) + 0.5f.xx;
}

float2 scSampleProbeDepthMV(uint probeIdx, float3 dirToSurface)
{
    float3 param = dirToSurface;
    float2 uv = scGatherOctEncode(param);
    uint2 texel = uint2(clamp(uint(uv.x * 8.0f), 0u, 7u), clamp(uint(uv.y * 8.0f), 0u, 7u));
    uint octIdx = (texel.y * 8u) + texel.x;
    uint base = probeIdx * 128u;
    return float2(asfloat(ddgiDepth.Load((base + octIdx) * 4 + 0)), asfloat(ddgiDepth.Load(((base + 64u) + octIdx) * 4 + 0)));
}

float scVisibilityWeight(float distToProbe, float mean, float variance)
{
    float dist = max(distToProbe - mean, 0.0f);
    if (variance < 9.9999997473787516355514526367188e-05f)
    {
        return 1.0f;
    }
    float cheb = variance / (variance + (dist * dist));
    return clamp(max(cheb * 2.0f, 0.20000000298023223876953125f), 0.0f, 1.0f);
}

float3 gatherProbeIrradiance(float3 worldPos, float3 N, out bool inVolume)
{
    inVolume = false;
    if (flp_params.ddgi_counts_valid.w == 0u)
    {
        return 0.0f.xxx;
    }
    float3 org = flp_params.ddgi_origin_spacing.xyz;
    float spacing = max(flp_params.ddgi_origin_spacing.w, 0.00999999977648258209228515625f);
    int3 counts = int3(flp_params.ddgi_counts_valid.xyz);
    float3 gp = (worldPos - org) / spacing.xxx;
    float3 gridMax = float3(counts - int3(1, 1, 1));
    bool _554 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _562;
    if (!_554)
    {
        _562 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _562 = _554;
    }
    if (_562)
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
                    pixOpen += (asfloat(ddgiOpenSky.Load((pidx * 2u) * 4 + 0)) * tw);
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
                float3 biasedPos = worldPos + ((N * spacing) * 0.0500000007450580596923828125f);
                float3 toProbeVec = probePos - biasedPos;
                float distToProbe = length(toProbeVec);
                float3 toProbe = toProbeVec / max(distToProbe, 9.9999997473787516355514526367188e-06f).xxx;
                uint param_2 = pidx_1;
                float3 param_3 = -toProbe;
                float2 depthMV = scSampleProbeDepthMV(param_2, param_3);
                float insidePenalty = 1.0f;
                float burial = 1.0f;
                if (depthMV.x < (spacing * 0.20000000298023223876953125f))
                {
                    insidePenalty = lerp(0.20000000298023223876953125f, 1.0f, smoothstep(0.0f, 0.20000000298023223876953125f, depthMV.x / spacing));
                    burial = lerp(0.100000001490116119384765625f, 1.0f, smoothstep(0.0f, 0.20000000298023223876953125f, depthMV.x / spacing));
                }
                float normalWeight = lerp(0.20000000298023223876953125f, 1.0f, clamp((dot(N, toProbe) + 0.5f) / 0.699999988079071044921875f, 0.0f, 1.0f));
                float nwSoft = max(normalWeight, 0.4000000059604644775390625f);
                float param_4 = distToProbe;
                float param_5 = depthMV.x;
                float param_6 = depthMV.y;
                float vw = max(scVisibilityWeight(param_4, param_5, param_6), 0.37999999523162841796875f);
                float atten = 1.0f;
                if (flp_params.radiosity_gate.w != 0u)
                {
                    float pOpen = asfloat(ddgiOpenSky.Load((pidx_1 * 2u) * 4 + 0));
                    float skyness = smoothstep(0.1500000059604644775390625f, 0.449999988079071044921875f, pOpen);
                    float enclosed = 1.0f - smoothstep(0.0500000007450580596923828125f, 0.25f, pixOpen);
                    float classify = 1.0f - ((skyness * enclosed) * 0.85000002384185791015625f);
                    if (flp_params.opensky_v2 != 0u)
                    {
                        _921 = lerp(1.0f, classify, asfloat(ddgiOpenSky.Load(((pidx_1 * 2u) + 1u) * 4 + 0)));
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
        result += ((accVis / visW.xxx) * 0.449999988079071044921875f);
    }
    if (lowW > 0.0f)
    {
        result += ((accLow / lowW.xxx) * 0.550000011920928955078125f);
    }
    if ((visW <= 0.0f) && (lowW <= 0.0f))
    {
        return 0.0f.xxx;
    }
    result *= 3.1415927410125732421875f;
    return min(result, 64.0f.xxx);
}

float scRadiosityWeight(uint bits)
{
    float _1023;
    if (bits != 0u)
    {
        _1023 = asfloat(bits);
    }
    else
    {
        _1023 = 0.25f;
    }
    return _1023;
}

void comp_main()
{
    uint global_tid = gl_GlobalInvocationID.x;
    if (global_tid >= flp_params.total_texels)
    {
        return;
    }
    uint lo = 0u;
    uint hi = flp_params.card_count;
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
    if (lo >= flp_params.card_count)
    {
        return;
    }
    uint local_texel = global_tid - card_dispatch.Load(lo * 32 + 0);
    uint res = card_dispatch.Load(lo * 32 + 8);
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_origin = uint2(card_dispatch.Load(lo * 32 + 12), card_dispatch.Load(lo * 32 + 16));
    if (flp_params.page_table_enabled != 0u)
    {
        uint ptEntry = pageTable.Load(card_dispatch.Load(lo * 32 + 20) * 4 + 0);
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
        if (albedo.w < 0.00999999977648258209228515625f)
        {
            return;
        }
    }
    else
    {
        bool _1200 = (transportBits & 128u) == 0u;
        bool _1206;
        if (!_1200)
        {
            _1206 = isnan(depth);
        }
        else
        {
            _1206 = _1200;
        }
        bool _1212;
        if (!_1206)
        {
            _1212 = isinf(depth);
        }
        else
        {
            _1212 = _1206;
        }
        bool _1215 = _1212 || (depth < 0.0f);
        bool _1223;
        if (!_1215)
        {
            _1223 = any(isnan(albedo));
        }
        else
        {
            _1223 = _1215;
        }
        bool _1230;
        if (!_1223)
        {
            _1230 = any(isinf(albedo));
        }
        else
        {
            _1230 = _1223;
        }
        if (_1230)
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
    _1304.center = asfloat(cards.Load4(card_idx * 64 + 0));
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
    float dir_visibility = asfloat(shadow_vis.Load((atlas_uv.x + (atlas_size * atlas_uv.y)) * 4 + 0));
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
            float3 to_light = asfloat(lights.Load4(li * 48 + 0)).xyz - world_pos;
            float dist = length(to_light);
            float radius = asfloat(lights.Load(li * 48 + 12));
            if (dist > radius)
            {
                continue;
            }
            L = to_light / max(dist, 0.001000000047497451305389404296875f).xxx;
            float r2 = radius * radius;
            attenuation = max(1.0f - ((dist * dist) / r2), 0.0f);
        }
        float signedNdotL = dot(normal, L);
        float frontTerm = max(signedNdotL, 0.0f);
        float backTerm = max(-signedNdotL, 0.0f) * backTransmittance;
        direct_light += ((asfloat(lights.Load4(li * 48 + 16)).xyz * (frontTerm + backTerm)) * attenuation);
    }
    float3 prev_light = prev_lighting_atlas[int2(atlas_uv)].xyz;
    bool _1455 = any(isnan(prev_light));
    bool _1462;
    if (!_1455)
    {
        _1462 = any(isinf(prev_light));
    }
    else
    {
        _1462 = _1455;
    }
    if (_1462)
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
        uint param_7 = flp_params.radiosity_gate.y;
        _1517 = tileRadiance * scRadiosityWeight(param_7);
    }
    else
    {
        float3 _1530;
        if (probeFed && probeInVolume)
        {
            uint param_8 = flp_params.radiosity_gate.y;
            _1530 = tileRadiance * scRadiosityWeight(param_8);
        }
        else
        {
            _1530 = tileRadiance + ((prev_light * albedo.xyz) * 0.3183098733425140380859375f);
        }
        _1517 = _1530;
    }
    float3 result = ((((direct_light + E_indirect) * albedo.xyz) * 0.3183098733425140380859375f) + _1517) + emissive;
    result = min(result, 64.0f.xxx);
    lighting_out[int2(atlas_uv)] = float4(result, 1.0f);
    float3 result_nodirect = result - ((direct_light * albedo.xyz) * 0.3183098733425140380859375f);
    result_nodirect = max(result_nodirect, 0.0f.xxx);
    result_nodirect = min(result_nodirect, 64.0f.xxx);
    nodirect_out[int2(atlas_uv)] = float4(result_nodirect, 1.0f);
}

[numthreads(256, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
