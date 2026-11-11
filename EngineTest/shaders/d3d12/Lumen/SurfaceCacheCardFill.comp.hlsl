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

static const uint3 gl_WorkGroupSize = uint3(256u, 1u, 1u);

cbuffer CardFillParamsBlock : register(b7, space0)
{
    float4 cfp_sdf_origins[3] : packoffset(c0);
    float4 cfp_sdf_extents[3] : packoffset(c3);
    uint4 cfp_sdf_resolutions_count : packoffset(c6);
    float4 cfp_light_direction : packoffset(c7);
    float4 cfp_light_color : packoffset(c8);
    uint cfp_total_texels : packoffset(c9);
    uint cfp_card_count : packoffset(c9.y);
    uint cfp_pad0 : packoffset(c9.z);
    uint cfp_pad1 : packoffset(c9.w);
};

ByteAddressBuffer inst : register(t10, space0);
ByteAddressBuffer mat : register(t11, space0);
ByteAddressBuffer cd : register(t8, space0);
ByteAddressBuffer cb : register(t9, space0);
Texture2DArray<float4> albedo_textures : register(t12, space0);
SamplerState albedo_sampler : register(s13, space0);
Texture3D<float4> sdf_cascade_0 : register(t0, space0);
Texture3D<float4> sdf_cascade_1 : register(t1, space0);
Texture3D<float4> sdf_cascade_2 : register(t2, space0);
RWTexture2D<unorm float4> albedo_atlas : register(u3, space0);
RWTexture2D<float> depth_atlas : register(u5, space0);
RWTexture2D<float2> normal_atlas : register(u4, space0);
RWTexture2D<float4> emissive_atlas : register(u16, space0);
RWTexture2D<float4> lighting_out : register(u6, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint3 spvTextureSize(Texture2DArray<float4> Tex, uint Level, out uint Param)
{
    uint3 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, ret.z, Param);
    return ret;
}

bool legacySurfacePolicy()
{
    return (cfp_pad0 & 2147483648u) != 0u;
}

bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    float3 local = pos - origin;
    bool _257 = local.x >= 0.0f;
    bool _265;
    if (_257)
    {
        _265 = local.x < extent.x;
    }
    else
    {
        _265 = _257;
    }
    bool _271;
    if (_265)
    {
        _271 = local.y >= 0.0f;
    }
    else
    {
        _271 = _265;
    }
    bool _279;
    if (_271)
    {
        _279 = local.y < extent.y;
    }
    else
    {
        _279 = _271;
    }
    bool _285;
    if (_279)
    {
        _285 = local.z >= 0.0f;
    }
    else
    {
        _285 = _279;
    }
    bool _293;
    if (_285)
    {
        _293 = local.z < extent.z;
    }
    else
    {
        _293 = _285;
    }
    return _293;
}

float sampleSDF(Texture3D<float4> tex, float3 pos, float3 origin, float3 extent, uint res)
{
    float3 uvw = (pos - origin) / extent;
    bool _304 = uvw.x < 0.0f;
    bool _311;
    if (!_304)
    {
        _311 = uvw.x > 1.0f;
    }
    else
    {
        _311 = _304;
    }
    bool _318;
    if (!_311)
    {
        _318 = uvw.y < 0.0f;
    }
    else
    {
        _318 = _311;
    }
    bool _325;
    if (!_318)
    {
        _325 = uvw.y > 1.0f;
    }
    else
    {
        _325 = _318;
    }
    bool _332;
    if (!_325)
    {
        _332 = uvw.z < 0.0f;
    }
    else
    {
        _332 = _325;
    }
    bool _339;
    if (!_332)
    {
        _339 = uvw.z > 1.0f;
    }
    else
    {
        _339 = _332;
    }
    if (_339)
    {
        return 10000000000.0f;
    }
    int r = int(res);
    int3 t = clamp(int3(uvw * float(r)), int3(0, 0, 0), (r - 1).xxx);
    return tex.Load(int4(t, 0)).x;
}

float sampleBestSDF(float3 pos)
{
    uint cc = cfp_sdf_resolutions_count.w;
    bool _372 = cc > 0u;
    bool _387;
    if (_372)
    {
        float3 param = pos;
        float3 param_1 = cfp_sdf_origins[0].xyz;
        float3 param_2 = cfp_sdf_extents[0].xyz;
        _387 = isInsideCascade(param, param_1, param_2);
    }
    else
    {
        _387 = _372;
    }
    if (_387)
    {
        float3 param_3 = pos;
        float3 param_4 = cfp_sdf_origins[0].xyz;
        float3 param_5 = cfp_sdf_extents[0].xyz;
        uint param_6 = cfp_sdf_resolutions_count.x;
        return sampleSDF(sdf_cascade_0, param_3, param_4, param_5, param_6);
    }
    bool _407 = cc > 1u;
    bool _421;
    if (_407)
    {
        float3 param_7 = pos;
        float3 param_8 = cfp_sdf_origins[1].xyz;
        float3 param_9 = cfp_sdf_extents[1].xyz;
        _421 = isInsideCascade(param_7, param_8, param_9);
    }
    else
    {
        _421 = _407;
    }
    if (_421)
    {
        float3 param_10 = pos;
        float3 param_11 = cfp_sdf_origins[1].xyz;
        float3 param_12 = cfp_sdf_extents[1].xyz;
        uint param_13 = cfp_sdf_resolutions_count.y;
        return sampleSDF(sdf_cascade_1, param_10, param_11, param_12, param_13);
    }
    bool _441 = cc > 2u;
    bool _455;
    if (_441)
    {
        float3 param_14 = pos;
        float3 param_15 = cfp_sdf_origins[2].xyz;
        float3 param_16 = cfp_sdf_extents[2].xyz;
        _455 = isInsideCascade(param_14, param_15, param_16);
    }
    else
    {
        _455 = _441;
    }
    if (_455)
    {
        float3 param_17 = pos;
        float3 param_18 = cfp_sdf_origins[2].xyz;
        float3 param_19 = cfp_sdf_extents[2].xyz;
        uint param_20 = cfp_sdf_resolutions_count.z;
        return sampleSDF(sdf_cascade_2, param_17, param_18, param_19, param_20);
    }
    return 10000000000.0f;
}

float3 getMeshTint(uint meshInstanceId)
{
    bool legacy = legacySurfacePolicy();
    bool _117 = !legacy;
    bool _125;
    if (!_117)
    {
        _125 = (cfp_pad0 & 2u) != 0u;
    }
    else
    {
        _125 = _117;
    }
    bool bits = _125;
    uint _131;
    inst.GetDimensions(_131);
    _131 = (_131 - 0) / 16;
    if (meshInstanceId >= (uint(int(_131)) / 12u))
    {
        return 0.0f.xxx;
    }
    float materialWord = asfloat(inst.Load(((meshInstanceId * 12u) + 8u) * 16 + 4));
    uint _152;
    if (bits)
    {
        _152 = asuint(materialWord);
    }
    else
    {
        _152 = uint(materialWord);
    }
    uint matId = _152;
    uint _166;
    mat.GetDimensions(_166);
    _166 = (_166 - 0) / 16;
    if (matId >= (uint(int(_166)) / 3u))
    {
        return 0.0f.xxx;
    }
    float textureWord = asfloat(mat.Load(((matId * 3u) + 0u) * 16 + 0));
    uint _182;
    if (bits)
    {
        _182 = asuint(textureWord);
    }
    else
    {
        _182 = uint(textureWord);
    }
    uint texIdx = _182;
    bool _193 = texIdx == 4294967295u;
    bool _207;
    if (!_193)
    {
        uint _203_dummy_parameter;
        _207 = texIdx >= uint(int3(spvTextureSize(albedo_textures, uint(0), _203_dummy_parameter)).z);
    }
    else
    {
        _207 = _193;
    }
    if (_207)
    {
        bool3 _213 = legacy.xxx;
        return float3(_213.x ? 0.5f.xxx.x : 0.0f.xxx.x, _213.y ? 0.5f.xxx.y : 0.0f.xxx.y, _213.z ? 0.5f.xxx.z : 0.0f.xxx.z);
    }
    float3 tex_color = albedo_textures.SampleLevel(albedo_sampler, float3(0.5f, 0.5f, float(texIdx)), 0.0f).xyz;
    float lum = dot(tex_color, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    if (legacy && (lum < 0.02999999932944774627685546875f))
    {
        return float3(0.4000000059604644775390625f, 0.37999999523162841796875f, 0.3499999940395355224609375f);
    }
    return tex_color;
}

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

void comp_main()
{
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= cfp_total_texels)
    {
        return;
    }
    uint lo = 0u;
    uint hi = cfp_card_count;
    while (lo < hi)
    {
        uint mid = (lo + hi) >> 1u;
        if ((cd.Load(mid * 32 + 0) + cd.Load(mid * 32 + 4)) <= tid)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }
    if (lo >= cfp_card_count)
    {
        return;
    }
    if ((cd.Load(lo * 32 + 24) & 1u) != 0u)
    {
        return;
    }
    SurfaceCacheCard _557;
    _557.center = asfloat(cb.Load4(cd.Load(lo * 32 + 20) * 64 + 0));
    _557.extent = asfloat(cb.Load4(cd.Load(lo * 32 + 20) * 64 + 16));
    _557.axis_direction = cb.Load(cd.Load(lo * 32 + 20) * 64 + 32);
    _557.resolution = cb.Load(cd.Load(lo * 32 + 20) * 64 + 36);
    _557.atlas_offset_x = cb.Load(cd.Load(lo * 32 + 20) * 64 + 40);
    _557.atlas_offset_y = cb.Load(cd.Load(lo * 32 + 20) * 64 + 44);
    _557.mesh_instance_id = cb.Load(cd.Load(lo * 32 + 20) * 64 + 48);
    _557.static_mesh_slot = cb.Load(cd.Load(lo * 32 + 20) * 64 + 52);
    _557.flags = cb.Load(cd.Load(lo * 32 + 20) * 64 + 56);
    _557.page_card = cb.Load(cd.Load(lo * 32 + 20) * 64 + 60);
    SurfaceCacheCard card;
    card.center = _557.center;
    card.extent = _557.extent;
    card.axis_direction = _557.axis_direction;
    card.resolution = _557.resolution;
    card.atlas_offset_x = _557.atlas_offset_x;
    card.atlas_offset_y = _557.atlas_offset_y;
    card.mesh_instance_id = _557.mesh_instance_id;
    card.static_mesh_slot = _557.static_mesh_slot;
    card.flags = _557.flags;
    card.page_card = _557.page_card;
    if (card.resolution == 0u)
    {
        return;
    }
    uint local_texel = tid - cd.Load(lo * 32 + 0);
    uint res = card.resolution;
    uint lx = local_texel % res;
    uint ly = local_texel / res;
    int2 atlas_uv = int2(int(card.atlas_offset_x + lx), int(card.atlas_offset_y + ly));
    float4 existing_albedo = albedo_atlas[atlas_uv];
    bool legacySurface = legacySurfacePolicy();
    float existingDepth = depth_atlas[atlas_uv].xxxx.x;
    uint existingPayload = uint(round(clamp(existing_albedo.w, 0.0f, 1.0f) * 255.0f));
    bool _649 = (existingPayload & 128u) != 0u;
    bool _655;
    if (_649)
    {
        _655 = !isnan(existingDepth);
    }
    else
    {
        _655 = _649;
    }
    bool _661;
    if (_655)
    {
        _661 = !isinf(existingDepth);
    }
    else
    {
        _661 = _655;
    }
    bool measuredSurface = _661 && (existingDepth >= 0.0f);
    bool _666;
    if (legacySurface)
    {
        _666 = existing_albedo.w > 0.5f;
    }
    else
    {
        _666 = measuredSurface;
    }
    if (_666)
    {
        return;
    }
    if (!legacySurface)
    {
        albedo_atlas[atlas_uv] = 0.0f.xxxx;
        normal_atlas[atlas_uv] = float4(0.5f, 0.5f, 0.0f, 0.0f).xy;
        depth_atlas[atlas_uv] = float4(-1.0f, 0.0f, 0.0f, 0.0f).x;
        emissive_atlas[atlas_uv] = 0.0f.xxxx;
        lighting_out[atlas_uv] = 0.0f.xxxx;
        return;
    }
    float u = (float(lx) + 0.5f) / float(res);
    float v = (float(ly) + 0.5f) / float(res);
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> uint(8)) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 right;
    float3 up;
    float3 fwd;
    float ex;
    float ey;
    if (axis == 0u)
    {
        right = float3(0.0f, 1.0f, 0.0f);
        up = float3(0.0f, 0.0f, 1.0f);
        fwd = float3(s, 0.0f, 0.0f);
        ex = card.extent.y;
        ey = card.extent.z;
    }
    else
    {
        if (axis == 1u)
        {
            right = float3(1.0f, 0.0f, 0.0f);
            up = float3(0.0f, 0.0f, 1.0f);
            fwd = float3(0.0f, s, 0.0f);
            ex = card.extent.x;
            ey = card.extent.z;
        }
        else
        {
            right = float3(1.0f, 0.0f, 0.0f);
            up = float3(0.0f, 1.0f, 0.0f);
            fwd = float3(0.0f, 0.0f, s);
            ex = card.extent.x;
            ey = card.extent.y;
        }
    }
    float3 card_center = card.center.xyz;
    float3 plane_pos = (card_center + (right * (((u - 0.5f) * ex) * 2.0f))) + (up * (((v - 0.5f) * ey) * 2.0f));
    float _794;
    if (cfp_pad1 != 0u)
    {
        _794 = card.extent[axis];
    }
    else
    {
        _794 = card.extent.x;
    }
    float max_depth = _794;
    float t = 0.0f;
    float hit_dist = -1.0f;
    float3 hit_pos = plane_pos;
    float voxel = max(cfp_sdf_extents[0].x / float(cfp_sdf_resolutions_count.x), 0.0500000007450580596923828125f);
    float hit_threshold = voxel * 0.5f;
    float minStep = voxel * 0.25f;
    float _884;
    for (uint _step = 0u; _step < 32u; _step++)
    {
        float3 p = plane_pos + (fwd * t);
        float3 param = p;
        float d = sampleBestSDF(param);
        if (d > 1000000000.0f)
        {
            t += (max_depth * 0.100000001490116119384765625f);
            if (t > max_depth)
            {
                break;
            }
            continue;
        }
        if (d < hit_threshold)
        {
            if (((cfp_pad0 & 1u) != 0u) && (t == 0.0f))
            {
                break;
            }
            hit_dist = t;
            hit_pos = p;
            break;
        }
        if (d > (hit_threshold * 4.0f))
        {
            _884 = d * 1.2000000476837158203125f;
        }
        else
        {
            _884 = max(d, minStep);
        }
        float advance = _884;
        t += advance;
        if (t > max_depth)
        {
            break;
        }
    }
    uint param_1 = card.mesh_instance_id;
    float3 mesh_tint = getMeshTint(param_1);
    mesh_tint = clamp(mesh_tint, 0.180000007152557373046875f.xxx, 1.0f.xxx);
    if (hit_dist < 0.0f)
    {
        albedo_atlas[atlas_uv] = 0.0f.xxxx;
        normal_atlas[atlas_uv] = float4(0.5f, 0.5f, 0.0f, 1.0f).xy;
        depth_atlas[atlas_uv] = float4(-1.0f, 0.0f, 0.0f, 1.0f).x;
        return;
    }
    float eps = voxel;
    float3 param_2 = hit_pos + float3(eps, 0.0f, 0.0f);
    float3 param_3 = hit_pos - float3(eps, 0.0f, 0.0f);
    float3 param_4 = hit_pos + float3(0.0f, eps, 0.0f);
    float3 param_5 = hit_pos - float3(0.0f, eps, 0.0f);
    float3 param_6 = hit_pos + float3(0.0f, 0.0f, eps);
    float3 param_7 = hit_pos - float3(0.0f, 0.0f, eps);
    float3 grad = float3(sampleBestSDF(param_2) - sampleBestSDF(param_3), sampleBestSDF(param_4) - sampleBestSDF(param_5), sampleBestSDF(param_6) - sampleBestSDF(param_7));
    float gradLen = length(grad);
    float3 _979;
    if (gradLen > 9.9999997473787516355514526367188e-05f)
    {
        _979 = normalize(-grad);
    }
    else
    {
        _979 = -fwd;
    }
    float3 N = _979;
    float3 L = normalize(-cfp_light_direction.xyz);
    float NdotL = max(dot(N, L), 0.0f);
    float3 direct = (cfp_light_color.xyz * cfp_light_color.w) * NdotL;
    float NdotUp = N.y;
    float3 sky_zenith = float3(0.20000000298023223876953125f, 0.25f, 0.4000000059604644775390625f);
    float3 sky_horizon = float3(0.3499999940395355224609375f, 0.319999992847442626953125f, 0.2800000011920928955078125f);
    float3 sky_nadir = float3(0.07999999821186065673828125f, 0.07999999821186065673828125f, 0.100000001490116119384765625f);
    float3 sky;
    if (NdotUp >= 0.0f)
    {
        sky = lerp(sky_horizon, sky_zenith, NdotUp.xxx);
    }
    else
    {
        sky = lerp(sky_horizon, sky_nadir, (-NdotUp).xxx);
    }
    float3 total_light = (direct + sky) * mesh_tint;
    total_light = max(total_light, float3(0.07999999821186065673828125f, 0.07999999821186065673828125f, 0.0900000035762786865234375f));
    albedo_atlas[atlas_uv] = float4(mesh_tint, 0.501960813999176025390625f);
    float3 param_8 = N;
    normal_atlas[atlas_uv] = float4(octEncode(param_8), 0.0f, 1.0f).xy;
    depth_atlas[atlas_uv] = float4(hit_dist, 0.0f, 0.0f, 1.0f).x;
    lighting_out[atlas_uv] = float4(total_light, 1.0f);
}

[numthreads(256, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
