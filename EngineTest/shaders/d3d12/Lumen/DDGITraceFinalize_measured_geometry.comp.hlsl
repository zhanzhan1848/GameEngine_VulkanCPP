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

struct SCGridParams
{
    float3 grid_min;
    float3 grid_max;
    uint3 dims;
    float cell_size;
    uint card_count;
};

struct TetraResult
{
    uint pi[4];
    float bw[4];
};

struct SurfaceCacheCardLookup
{
    float4 aabb_min;
    float4 aabb_max;
    uint card_start;
    uint card_count;
    uint _pad0;
    uint _pad1;
};

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

ByteAddressBuffer scgo : register(t11, space0);
cbuffer DDGIVolumeData : register(b1, space0)
{
    float4 volume_ProbeOrigin : packoffset(c0);
    float volume_ProbeSpacing : packoffset(c1);
    float volume_pad_before_counts_0 : packoffset(c1.y);
    float volume_pad_before_counts_1 : packoffset(c1.z);
    float volume_pad_before_counts_2 : packoffset(c1.w);
    uint4 volume_ProbeCounts : packoffset(c2);
    uint volume_RaysPerProbe : packoffset(c3);
    uint volume_ProbeCountTotal : packoffset(c3.y);
    float volume_IrradianceBlurSigma : packoffset(c3.z);
    float volume_DepthBlurSigma : packoffset(c3.w);
    float volume_DeltaTime : packoffset(c4);
    uint volume_FrameIndex : packoffset(c4.y);
    float volume_RayMaxDistance : packoffset(c4.z);
    float volume_ProbeHysteresis : packoffset(c4.w);
    float volume_TemporalAlpha : packoffset(c5);
    uint volume_ProbeUpdateCount : packoffset(c5.y);
    uint volume_SCLookupCount : packoffset(c5.z);
    uint volume_SCAtlasSize : packoffset(c5.w);
    int volume_ProbeRelocationShiftX : packoffset(c6);
    int volume_ProbeRelocationShiftY : packoffset(c6.y);
    int volume_ProbeRelocationShiftZ : packoffset(c6.z);
    int volume_RaySampleMode : packoffset(c6.w);
    float4 volume_SdfOrigins[3] : packoffset(c7);
    float4 volume_SdfVoxelSizes[3] : packoffset(c10);
    float4 volume_SdfExtents[3] : packoffset(c13);
    uint4 volume_SdfResolutionsAndCount : packoffset(c16);
    float4 volume_LightDirection : packoffset(c17);
    float4 volume_LightColor : packoffset(c18);
    float4 volume_SkyColor : packoffset(c19);
    float4 volume_Albedo : packoffset(c20);
};

cbuffer ViewProjBlock : register(b7, space0)
{
    row_major float4x4 vp_viewProjection : packoffset(c0);
    row_major float4x4 vp_radianceViewProjection : packoffset(c4);
    row_major float4x4 vp_inverseViewProjection : packoffset(c8);
    row_major float4x4 vp_inverseRadianceViewProjection : packoffset(c12);
    float4 vp_screenValidity : packoffset(c16);
};

ByteAddressBuffer ih : register(t10, space0);
ByteAddressBuffer pul : register(t3, space0);
ByteAddressBuffer hd : register(t2, space0);
RWByteAddressBuffer hnb : register(u14, space0);
ByteAddressBuffer robuf : register(t17, space0);
ByteAddressBuffer cl : register(t5, space0);
ByteAddressBuffer cd : register(t6, space0);
ByteAddressBuffer scgc : register(t12, space0);
RWByteAddressBuffer rb : register(u4, space0);
Texture2D<float4> lighting_atlas : register(t0, space0);
Texture2D<float4> radiance_depth : register(t16, space0);
Texture2D<float4> prev_frame_color : register(t8, space0);
Texture2D<float4> screen_depth : register(t15, space0);
Texture2D<float4> gbuffer_albedo : register(t9, space0);
Texture2D<float4> depth_atlas : register(t13, space0);

static uint3 gl_WorkGroupID;
static uint3 gl_LocalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint3 gl_LocalInvocationID : SV_GroupThreadID;
};

uint2 spvTextureSize(Texture2D<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

uint3 ddgiProbeGridCoord(uint probeIdx, uint3 counts)
{
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    return uint3(px, py, pz);
}

float3 ddgiProbeWorldPos(uint3 gc, float3 origin, float spacing)
{
    return origin + (float3(float(gc.x), float(gc.y), float(gc.z)) * spacing);
}

uint ddgiOctSampleHash(inout uint x)
{
    x ^= (x >> 16u);
    x *= 2146121005u;
    x ^= (x >> 15u);
    x *= 2221713035u;
    return x ^ (x >> 16u);
}

float4 ddgiOctStratifiedSample(uint rayIndex, uint rayCount, uint frameIndex)
{
    uint cell = rayIndex / (rayCount / 64u);
    uint param = frameIndex ^ 2654435769u;
    uint _1160 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _1165 = ddgiOctSampleHash(param_1);
    uint seed = _1160 ^ _1165;
    uint param_2 = seed ^ 1757159915u;
    uint _1172 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _1180 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_1172 >> 14u), float(_1180 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _1225 = q;
        float3 _1232 = q;
        bool2 _1235 = bool2(_1232.xy.x >= 0.0f.xx.x, _1232.xy.y >= 0.0f.xx.y);
        float2 _1237 = (1.0f.xx - abs(_1225.yx)) * float2(_1235.x ? 1.0f.xx.x : (-1.0f).xx.x, _1235.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _1237.x;
        q.y = _1237.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _1266 = rayCount >= 64u;
        bool _1272;
        if (_1266)
        {
            _1272 = (rayCount % 64u) == 0u;
        }
        else
        {
            _1272 = _1266;
        }
        if (_1272)
        {
            uint param = rayIndex;
            uint param_1 = rayCount;
            uint param_2 = frameIndex;
            return ddgiOctStratifiedSample(param, param_1, param_2);
        }
        mode = 0u;
    }
    float u = frac((float(rayIndex) + 0.5f) * 0.61803400516510009765625f);
    float v = frac(((float(rayIndex) + 0.5f) * 0.61803400516510009765625f) * 0.61803400516510009765625f);
    if ((((mode != 0u) && (mode != 3u)) && (mode != 4u)) && (mode != 5u))
    {
        u = frac(u + (float(frameIndex) * 0.61803400516510009765625f));
        v = frac(v + ((float(frameIndex) * 0.61803400516510009765625f) * 0.61803400516510009765625f));
    }
    if (mode == 3u)
    {
        v = (float(rayIndex) + 0.5f) / max(float(rayCount), 1.0f);
    }
    if (mode == 2u)
    {
        float3 w = normalize(toLight);
        float s = (w.z >= 0.0f) ? 1.0f : (-1.0f);
        float a = (-1.0f) / (s + w.z);
        float b = (w.x * w.y) * a;
        float3 tangent = float3(1.0f + (((s * w.x) * w.x) * a), s * b, (-s) * w.x);
        float3 bitangent = float3(b, s + ((w.y * w.y) * a), -w.y);
        float3 dir;
        if (v < 0.5f)
        {
            float xi = v * 2.0f;
            float cosT = sqrt(max(1.0f - xi, 0.0f));
            float sinT = sqrt(max(1.0f - (cosT * cosT), 0.0f));
            float th = 6.283185482025146484375f * u;
            dir = ((w * cosT) + (tangent * (sinT * cos(th)))) + (bitangent * (sinT * sin(th)));
        }
        else
        {
            float vu = (v - 0.5f) * 2.0f;
            float ph = acos(clamp(1.0f - (2.0f * vu), -1.0f, 1.0f));
            float sp = sin(ph);
            float th_1 = 6.283185482025146484375f * u;
            dir = float3(sp * cos(th_1), sp * sin(th_1), cos(ph));
        }
        float cosL = max(dot(dir, w), 0.0f);
        float pdf = max(((0.5f * cosL) / 3.1415927410125732421875f) + 0.0397887341678142547607421875f, 9.9999997473787516355514526367188e-06f);
        return float4(dir, pdf);
    }
    float theta = 6.283185482025146484375f * u;
    float phi = acos(clamp(1.0f - (2.0f * v), -1.0f, 1.0f));
    float sinPhi = sin(phi);
    return float4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi), 0.079577468335628509521484375f);
}

bool projectVisibleHit(float3 hitPos, float4x4 viewProjection, float4x4 inverseVP, Texture2D<float4> depthTexture, inout int2 texel, out uint reject)
{
    reject = 10u;
    float4 clip = mul(float4(hitPos, 1.0f), viewProjection);
    if (clip.w <= 0.0f)
    {
        return false;
    }
    float3 ndc = clip.xyz / clip.w.xxx;
    reject = 11u;
    bool _1764 = any(bool3(ndc.x < float3(-1.0f, -1.0f, 0.0f).x, ndc.y < float3(-1.0f, -1.0f, 0.0f).y, ndc.z < float3(-1.0f, -1.0f, 0.0f).z));
    bool _1772;
    if (!_1764)
    {
        _1772 = any(bool3(ndc.x > 1.0f.xxx.x, ndc.y > 1.0f.xxx.y, ndc.z > 1.0f.xxx.z));
    }
    else
    {
        _1772 = _1764;
    }
    if (_1772)
    {
        return false;
    }
    uint _1778_dummy_parameter;
    int2 dims = int2(spvTextureSize(depthTexture, uint(0), _1778_dummy_parameter));
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, 0.5f - (ndc.y * 0.5f));
    texel = clamp(int2(uv * float2(dims)), int2(0, 0), dims - int2(1, 1));
    float depth = depthTexture.Load(int3(texel, 0)).x;
    reject = 12u;
    if ((depth >= 1.0f) || (depth < 0.0f))
    {
        return false;
    }
    float2 center = (float2(texel) + 0.5f.xx) / float2(dims);
    float4 surface = mul(float4((center.x * 2.0f) - 1.0f, 1.0f - (center.y * 2.0f), depth, 1.0f), inverseVP);
    reject = 13u;
    if (abs(surface.w) < 9.9999999747524270787835121154785e-07f)
    {
        return false;
    }
    reject = 14u;
    return distance(surface.xyz / surface.w.xxx, hitPos) <= vp_screenValidity.z;
}

float3 sampleHitAlbedo(float3 hitPos, out uint source)
{
    source = 1u;
    if (vp_screenValidity.x < 0.5f)
    {
        return volume_Albedo.xyz;
    }
    float3 param = hitPos;
    float4x4 param_1 = vp_viewProjection;
    float4x4 param_2 = vp_inverseViewProjection;
    int2 param_3;
    uint param_4;
    bool _1940 = projectVisibleHit(param, param_1, param_2, screen_depth, param_3, param_4);
    int2 texel = param_3;
    source = param_4;
    if (!_1940)
    {
        return volume_Albedo.xyz;
    }
    source = 3u;
    uint _1952_dummy_parameter;
    int2 _1952 = int2(spvTextureSize(gbuffer_albedo, uint(0), _1952_dummy_parameter));
    uint _1954_dummy_parameter;
    int2 _1954 = int2(spvTextureSize(screen_depth, uint(0), _1954_dummy_parameter));
    if (any(bool2(_1952.x != _1954.x, _1952.y != _1954.y)))
    {
        return volume_Albedo.xyz;
    }
    source = 4u;
    return clamp(gbuffer_albedo.Load(int3(texel, 0)).xyz, 0.0f.xxx, 1.0f.xxx);
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

SCGridParams scGridLoad()
{
    uint4 param = uint4(scgo.Load(0), scgo.Load(4), scgo.Load(8), scgo.Load(12));
    uint4 param_1 = uint4(scgo.Load(16), scgo.Load(20), scgo.Load(24), scgo.Load(28));
    uint4 param_2 = uint4(scgo.Load(32), scgo.Load(36), scgo.Load(40), scgo.Load(44));
    return scGridDecode(param, param_1, param_2);
}

uint scGridCellIndex(SCGridParams gp, float3 p)
{
    float3 g = (p - gp.grid_min) / gp.cell_size.xxx;
    bool _873 = g.x < 0.0f;
    bool _880;
    if (!_873)
    {
        _880 = g.y < 0.0f;
    }
    else
    {
        _880 = _873;
    }
    bool _887;
    if (!_880)
    {
        _887 = g.z < 0.0f;
    }
    else
    {
        _887 = _880;
    }
    if (_887)
    {
        return 4294967295u;
    }
    uint3 c = uint3(g);
    bool _899 = c.x >= gp.dims.x;
    bool _908;
    if (!_899)
    {
        _908 = c.y >= gp.dims.y;
    }
    else
    {
        _908 = _899;
    }
    bool _917;
    if (!_908)
    {
        _917 = c.z >= gp.dims.z;
    }
    else
    {
        _917 = _908;
    }
    if (_917)
    {
        return 4294967295u;
    }
    return c.x + (gp.dims.x * (c.y + (gp.dims.y * c.z)));
}

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _601;
    if (axis == 0u)
    {
        _601 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _609;
        if (axis == 1u)
        {
            _609 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _609 = float3(0.0f, 0.0f, s);
        }
        _601 = _609;
    }
    return _601;
}

bool worldToCardUVMarginPad(float3 world_pos, SurfaceCacheCard card, inout float2 out_uv, float margin_uv, float pad_m)
{
    float3 local = world_pos - card.center.xyz;
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
    if (depth < 0.0f)
    {
        return false;
    }
    float padU = 0.0f;
    float padV = 0.0f;
    if (pad_m > 0.0f)
    {
        float _434;
        if (axis == 0u)
        {
            _434 = card.extent.y;
        }
        else
        {
            float _442;
            if (axis == 1u)
            {
                _442 = card.extent.x;
            }
            else
            {
                _442 = card.extent.x;
            }
            _434 = _442;
        }
        float eu = _434;
        float _455;
        if (axis == 0u)
        {
            _455 = card.extent.z;
        }
        else
        {
            float _463;
            if (axis == 1u)
            {
                _463 = card.extent.z;
            }
            else
            {
                _463 = card.extent.y;
            }
            _455 = _463;
        }
        float ev = _455;
        float _476;
        if (eu > 9.9999997473787516355514526367188e-05f)
        {
            _476 = pad_m / (eu * 2.0f);
        }
        else
        {
            _476 = 0.0f;
        }
        padU = _476;
        float _487;
        if (ev > 9.9999997473787516355514526367188e-05f)
        {
            _487 = pad_m / (ev * 2.0f);
        }
        else
        {
            _487 = 0.0f;
        }
        padV = _487;
    }
    bool _501 = u < ((-margin_uv) - padU);
    bool _511;
    if (!_501)
    {
        _511 = u >= ((1.0f + margin_uv) + padU);
    }
    else
    {
        _511 = _501;
    }
    bool _521;
    if (!_511)
    {
        _521 = v < ((-margin_uv) - padV);
    }
    else
    {
        _521 = _511;
    }
    bool _531;
    if (!_521)
    {
        _531 = v >= ((1.0f + margin_uv) + padV);
    }
    else
    {
        _531 = _521;
    }
    if (_531)
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999998986721038818359375f);
    v = clamp(v, 0.0f, 0.999998986721038818359375f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

bool worldToCardUVMargin(float3 world_pos, SurfaceCacheCard card, out float2 out_uv, float margin_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = margin_uv;
    float param_4 = 0.0f;
    float2 param_2;
    bool _571 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    out_uv = param_2;
    return _571;
}

bool worldToCardUV(float3 world_pos, SurfaceCacheCard card, out float2 out_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 0.0f;
    float2 param_2;
    bool _581 = worldToCardUVMargin(param, param_1, param_2, param_3);
    out_uv = param_2;
    return _581;
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

bool ddgiMeasuredOwnerCandidate(SurfaceCacheCard card, uint owner, bool ownerKnown, float3 hitPos, float3 geometricN, float3 rayDir, out float3 radiance, out float key, out uint reject)
{
    radiance = 0.0f.xxx;
    key = 1000000015047466219876688855040.0f;
    reject = 11u;
    bool _2500 = !ownerKnown;
    bool _2508;
    if (!_2500)
    {
        _2508 = card.mesh_instance_id != owner;
    }
    else
    {
        _2508 = _2500;
    }
    if (_2508)
    {
        return false;
    }
    bool _2514 = any(isnan(hitPos));
    bool _2521;
    if (!_2514)
    {
        _2521 = any(isinf(hitPos));
    }
    else
    {
        _2521 = _2514;
    }
    bool _2528;
    if (!_2521)
    {
        _2528 = any(isnan(geometricN));
    }
    else
    {
        _2528 = _2521;
    }
    bool _2535;
    if (!_2528)
    {
        _2535 = any(isinf(geometricN));
    }
    else
    {
        _2535 = _2528;
    }
    bool _2543;
    if (!_2535)
    {
        _2543 = dot(geometricN, geometricN) < 0.25f;
    }
    else
    {
        _2543 = _2535;
    }
    bool _2551;
    if (!_2543)
    {
        _2551 = dot(geometricN, rayDir) >= 0.0f;
    }
    else
    {
        _2551 = _2543;
    }
    bool _2558;
    if (!_2551)
    {
        _2558 = card.resolution == 0u;
    }
    else
    {
        _2558 = _2551;
    }
    bool _2566;
    if (!_2558)
    {
        _2566 = (card.axis_direction & 255u) > 2u;
    }
    else
    {
        _2566 = _2558;
    }
    bool _2575;
    if (!_2566)
    {
        _2575 = ((card.axis_direction >> 8u) & 255u) > 1u;
    }
    else
    {
        _2575 = _2566;
    }
    if (_2575)
    {
        return false;
    }
    float3 n = normalize(geometricN);
    SurfaceCacheCard param = card;
    float3 face = scCardFaceNormal(param);
    bool _2589 = dot(face, n) > (-0.5f);
    bool _2598;
    if (!_2589)
    {
        _2598 = dot(face, -rayDir) >= 0.0f;
    }
    else
    {
        _2598 = _2589;
    }
    if (_2598)
    {
        reject = 12u;
        return false;
    }
    float3 param_1 = hitPos;
    SurfaceCacheCard param_2 = card;
    float2 param_3;
    bool _2608 = worldToCardUV(param_1, param_2, param_3);
    float2 uv = param_3;
    bool _2610 = !_2608;
    bool _2617;
    if (!_2610)
    {
        _2617 = any(isnan(uv));
    }
    else
    {
        _2617 = _2610;
    }
    bool _2624;
    if (!_2617)
    {
        _2624 = any(isinf(uv));
    }
    else
    {
        _2624 = _2617;
    }
    if (_2624)
    {
        reject = 13u;
        return false;
    }
    reject = 14u;
    uint _2631_dummy_parameter;
    int2 ds = int2(spvTextureSize(depth_atlas, uint(0), _2631_dummy_parameter));
    uint _2634_dummy_parameter;
    int2 ls = int2(spvTextureSize(lighting_atlas, uint(0), _2634_dummy_parameter));
    bool _2637 = any(bool2(ds.x <= int2(0, 0).x, ds.y <= int2(0, 0).y));
    bool _2645;
    if (!_2637)
    {
        _2645 = any(bool2(ds.x != ls.x, ds.y != ls.y));
    }
    else
    {
        _2645 = _2637;
    }
    bool _2655;
    if (!_2645)
    {
        _2655 = card.atlas_offset_x >= uint(ds.x);
    }
    else
    {
        _2655 = _2645;
    }
    bool _2665;
    if (!_2655)
    {
        _2665 = card.atlas_offset_y >= uint(ds.y);
    }
    else
    {
        _2665 = _2655;
    }
    bool _2678;
    if (!_2665)
    {
        _2678 = card.resolution > (uint(ds.x) - card.atlas_offset_x);
    }
    else
    {
        _2678 = _2665;
    }
    bool _2691;
    if (!_2678)
    {
        _2691 = card.resolution > (uint(ds.y) - card.atlas_offset_y);
    }
    else
    {
        _2691 = _2678;
    }
    if (_2691)
    {
        return false;
    }
    uint2 texel = uint2(clamp(uv, float2(float(card.atlas_offset_x), float(card.atlas_offset_y)), float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth = depth_atlas.Load(int3(int2(texel), 0)).x;
    float4 light = lighting_atlas.Load(int3(int2(texel), 0));
    bool _2733 = isnan(depth);
    bool _2739;
    if (!_2733)
    {
        _2739 = isinf(depth);
    }
    else
    {
        _2739 = _2733;
    }
    bool _2742 = _2739 || (depth < 0.0f);
    bool _2750;
    if (!_2742)
    {
        _2750 = any(isnan(light));
    }
    else
    {
        _2750 = _2742;
    }
    bool _2757;
    if (!_2750)
    {
        _2757 = any(isinf(light));
    }
    else
    {
        _2757 = _2750;
    }
    bool _2764;
    if (!_2757)
    {
        _2764 = light.w <= 0.5f;
    }
    else
    {
        _2764 = _2757;
    }
    if (_2764)
    {
        reject = 14u;
        return false;
    }
    uint2 param_4 = texel;
    float param_5 = depth;
    SurfaceCacheCard param_6 = card;
    float3 captured = cardTexelToWorld(param_4, param_5, param_6);
    float3 delta = captured - hitPos;
    float along = dot(delta, n);
    float footprint = (2.0f * length(card.extent.xyz)) / float(card.resolution);
    bool _2796 = any(isnan(captured));
    bool _2803;
    if (!_2796)
    {
        _2803 = any(isinf(captured));
    }
    else
    {
        _2803 = _2796;
    }
    bool _2811;
    if (!_2803)
    {
        _2811 = abs(along) > 0.00200000009499490261077880859375f;
    }
    else
    {
        _2811 = _2803;
    }
    bool _2824;
    if (!_2811)
    {
        _2824 = length(delta - (n * along)) > max(footprint, 0.00200000009499490261077880859375f);
    }
    else
    {
        _2824 = _2811;
    }
    if (_2824)
    {
        reject = 15u;
        return false;
    }
    radiance = light.xyz;
    key = length(delta - (n * along)) + abs(along);
    reject = 0u;
    return true;
}

bool scQueryCardCandidateDbg(SurfaceCacheCard card, float3 hitPos, float3 surfaceFacing, float tolFloor, float qpadM, Texture2D<float4> depthAtlas, Texture2D<float4> lightingAtlas, float3 hitNormalHint, out float3 outRadiance, out float outKey, out uint outReject)
{
    outRadiance = 0.0f.xxx;
    outKey = 1000000015047466219876688855040.0f;
    outReject = 0u;
    float3 param = hitPos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 1.0f / float(max(card.resolution, 1u));
    float param_4 = qpadM;
    float2 param_2;
    bool _637 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    float2 atlasUV = param_2;
    if (!_637)
    {
        outReject = 1u;
        return false;
    }
    SurfaceCacheCard param_5 = card;
    if (dot(scCardFaceNormal(param_5), surfaceFacing) >= 0.0f)
    {
        outReject = 2u;
        return false;
    }
    float hintPenalty = 0.0f;
    if (dot(hitNormalHint, hitNormalHint) > 0.25f)
    {
        SurfaceCacheCard param_6 = card;
        float d = dot(scCardFaceNormal(param_6), hitNormalHint);
        if (d > (-0.5f))
        {
            outReject = 7u;
        }
        hintPenalty = max(d + 1.0f, 0.0f);
    }
    uint2 texel = uint2(clamp(atlasUV, 0.0f.xx, float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
    float depth = depthAtlas.Load(int3(int2(texel), 0)).x;
    bool _702 = isnan(depth);
    bool _708;
    if (!_702)
    {
        _708 = isinf(depth);
    }
    else
    {
        _708 = _702;
    }
    if (_708 || (depth < 0.0f))
    {
        outReject = 3u;
        return false;
    }
    uint2 param_7 = texel;
    float param_8 = depth;
    SurfaceCacheCard param_9 = card;
    float3 captured = cardTexelToWorld(param_7, param_8, param_9);
    float3 delta = captured - hitPos;
    SurfaceCacheCard param_10 = card;
    float3 n = scCardFaceNormal(param_10);
    float along = dot(delta, n);
    float3 perpV = delta - (n * along);
    float perp = length(perpV);
    float footprint = length(card.extent.xyz) / float(max(card.resolution, 1u));
    float tolAlong = tolFloor + footprint;
    float tolPerp = max(footprint * 1.5f, tolFloor);
    if (abs(along) > tolAlong)
    {
        outReject = 4u;
        return false;
    }
    if (perp > tolPerp)
    {
        outReject = 5u;
        return false;
    }
    float3 c = lightingAtlas.Load(int3(int2(texel), 0)).xyz;
    bool _788 = any(isnan(c));
    bool _795;
    if (!_788)
    {
        _795 = any(isinf(c));
    }
    else
    {
        _795 = _788;
    }
    if (_795)
    {
        outReject = 6u;
        return false;
    }
    outRadiance = c;
    outKey = (perp + (0.5f * abs(along))) + (hintPenalty * ((perp + abs(along)) + max(tolFloor, 0.0500000007450580596923828125f)));
    return true;
}

bool scQueryCardCandidate(SurfaceCacheCard card, float3 hitPos, float3 surfaceFacing, float tolFloor, Texture2D<float4> depthAtlas, Texture2D<float4> lightingAtlas, float3 hitNormalHint, out float3 outRadiance, out float outKey)
{
    SurfaceCacheCard param = card;
    float3 param_1 = hitPos;
    float3 param_2 = surfaceFacing;
    float param_3 = tolFloor;
    float param_4 = 0.0f;
    float3 param_5 = hitNormalHint;
    float3 param_6;
    float param_7;
    uint param_8;
    bool _834 = scQueryCardCandidateDbg(param, param_1, param_2, param_3, param_4, depthAtlas, lightingAtlas, param_5, param_6, param_7, param_8);
    outRadiance = param_6;
    outKey = param_7;
    uint rejectDummy = param_8;
    return _834;
}

bool samplePrevColor(float3 hitPos, inout float3 radiance)
{
    radiance = 0.0f.xxx;
    if (vp_screenValidity.y < 0.5f)
    {
        return false;
    }
    float3 param = hitPos;
    float4x4 param_1 = vp_radianceViewProjection;
    float4x4 param_2 = vp_inverseRadianceViewProjection;
    int2 param_3;
    uint param_4;
    bool _1879 = projectVisibleHit(param, param_1, param_2, radiance_depth, param_3, param_4);
    int2 texel = param_3;
    uint reject = param_4;
    if (!_1879)
    {
        return false;
    }
    uint _1888_dummy_parameter;
    int2 _1888 = int2(spvTextureSize(prev_frame_color, uint(0), _1888_dummy_parameter));
    uint _1890_dummy_parameter;
    int2 _1890 = int2(spvTextureSize(radiance_depth, uint(0), _1890_dummy_parameter));
    if (any(bool2(_1888.x != _1890.x, _1888.y != _1890.y)))
    {
        return false;
    }
    radiance = prev_frame_color.Load(int3(texel, 0)).xyz;
    bool _1902 = any(isnan(radiance));
    bool _1909;
    if (!_1902)
    {
        _1909 = any(isinf(radiance));
    }
    else
    {
        _1909 = _1902;
    }
    if (_1909)
    {
        return false;
    }
    radiance = max(radiance, 0.0f.xxx);
    return true;
}

TetraResult tetrahedral(float3 gp, uint3 gd)
{
    uint3 b0 = clamp(uint3(floor(gp)), uint3(0u, 0u, 0u), gd - uint3(1u, 1u, 1u));
    uint3 b1 = min((b0 + uint3(1u, 1u, 1u)), (gd - uint3(1u, 1u, 1u)));
    uint gx = gd.x;
    uint gxy = gd.x * gd.y;
    uint p[8];
    p[0] = (b0.x + (b0.y * gx)) + (b0.z * gxy);
    p[1] = (b1.x + (b0.y * gx)) + (b0.z * gxy);
    p[2] = (b0.x + (b1.y * gx)) + (b0.z * gxy);
    p[3] = (b1.x + (b1.y * gx)) + (b0.z * gxy);
    p[4] = (b0.x + (b0.y * gx)) + (b1.z * gxy);
    p[5] = (b1.x + (b0.y * gx)) + (b1.z * gxy);
    p[6] = (b0.x + (b1.y * gx)) + (b1.z * gxy);
    p[7] = (b1.x + (b1.y * gx)) + (b1.z * gxy);
    float fx = frac(gp.x);
    float fy = frac(gp.y);
    float fz = frac(gp.z);
    TetraResult r;
    if ((fx >= fy) && (fy >= fz))
    {
        r.pi[0] = p[0];
        r.pi[1] = p[1];
        r.pi[2] = p[3];
        r.pi[3] = p[7];
        r.bw[0] = 1.0f - fx;
        r.bw[1] = fx - fy;
        r.bw[2] = fy - fz;
        r.bw[3] = fz;
    }
    else
    {
        if ((fx >= fz) && (fz >= fy))
        {
            r.pi[0] = p[0];
            r.pi[1] = p[1];
            r.pi[2] = p[5];
            r.pi[3] = p[7];
            r.bw[0] = 1.0f - fx;
            r.bw[1] = fx - fz;
            r.bw[2] = fz - fy;
            r.bw[3] = fy;
        }
        else
        {
            if ((fy >= fx) && (fx >= fz))
            {
                r.pi[0] = p[0];
                r.pi[1] = p[2];
                r.pi[2] = p[3];
                r.pi[3] = p[7];
                r.bw[0] = 1.0f - fy;
                r.bw[1] = fy - fx;
                r.bw[2] = fx - fz;
                r.bw[3] = fz;
            }
            else
            {
                if ((fy >= fz) && (fz >= fx))
                {
                    r.pi[0] = p[0];
                    r.pi[1] = p[2];
                    r.pi[2] = p[6];
                    r.pi[3] = p[7];
                    r.bw[0] = 1.0f - fy;
                    r.bw[1] = fy - fz;
                    r.bw[2] = fz - fx;
                    r.bw[3] = fx;
                }
                else
                {
                    if ((fz >= fx) && (fx >= fy))
                    {
                        r.pi[0] = p[0];
                        r.pi[1] = p[4];
                        r.pi[2] = p[5];
                        r.pi[3] = p[7];
                        r.bw[0] = 1.0f - fz;
                        r.bw[1] = fz - fx;
                        r.bw[2] = fx - fy;
                        r.bw[3] = fy;
                    }
                    else
                    {
                        r.pi[0] = p[0];
                        r.pi[1] = p[4];
                        r.pi[2] = p[6];
                        r.pi[3] = p[7];
                        r.bw[0] = 1.0f - fz;
                        r.bw[1] = fz - fy;
                        r.bw[2] = fy - fx;
                        r.bw[3] = fx;
                    }
                }
            }
        }
    }
    for (uint i = 0u; i < 4u; i++)
    {
        r.bw[i] = max(r.bw[i], 0.0f);
    }
    return r;
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

float3 evalProbeRadianceSH9(float3 sh[9], float3 d)
{
    float3 _1028 = ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.48860251903533935546875f) * d.y))) + (sh[2] * (0.48860251903533935546875f * d.z))) + (sh[3] * ((-0.48860251903533935546875f) * d.x))) + (sh[4] * ((1.09254848957061767578125f * d.y) * d.x))) + (sh[5] * (((-1.09254848957061767578125f) * d.y) * d.z))) + (sh[6] * (0.3153915703296661376953125f * (((3.0f * d.z) * d.z) - 1.0f)))) + (sh[7] * (((-1.09254848957061767578125f) * d.x) * d.z))) + (sh[8] * (0.5462741851806640625f * ((d.x * d.x) - (d.y * d.y))));
    return _1028;
}

float3 shDot9(float3 c[9], float3 d)
{
    float3 param[9] = c;
    float3 param_1 = d;
    return evalProbeRadianceSH9(param, param_1);
}

float3 sampleDiffuseHistoryIrradianceOverPi(float3 pos, float3 N)
{
    float3 origin = volume_ProbeOrigin.xyz;
    float spacing = volume_ProbeSpacing;
    uint3 counts = volume_ProbeCounts.xyz;
    float3 gp = (pos - origin) / spacing.xxx;
    float3 gridMax = float3(float(counts.x - 1u), float(counts.y - 1u), float(counts.z - 1u));
    bool _2378 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _2386;
    if (!_2378)
    {
        _2386 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _2386 = _2378;
    }
    if (_2386)
    {
        return 0.0f.xxx;
    }
    float3 param = gp;
    uint3 param_1 = counts;
    TetraResult tet = tetrahedral(param, param_1);
    float3 result = 0.0f.xxx;
    float totalWeight = 0.0f;
    float3 sh[9];
    float3 _2447;
    for (uint p = 0u; p < 4u; p++)
    {
        if (tet.bw[p] < 0.001000000047497451305389404296875f)
        {
            continue;
        }
        uint base = tet.pi[p] * 9u;
        for (uint i = 0u; i < 9u; i++)
        {
            sh[i] = asfloat(ih.Load4((base + i) * 16 + 0)).xyz;
        }
        if (volume_SkyColor.w == 2.0f)
        {
            float3 param_2[9] = sh;
            float3 param_3 = N;
            _2447 = evalProbeIrradianceSH9(param_2, param_3);
        }
        else
        {
            float3 param_4[9] = sh;
            float3 param_5 = N;
            _2447 = shDot9(param_4, param_5);
        }
        float3 irradiance = _2447;
        result += (irradiance * tet.bw[p]);
        totalWeight += tet.bw[p];
    }
    if (totalWeight > 0.0f)
    {
        float3 value = result / totalWeight.xxx;
        float3 _2488;
        if (volume_SkyColor.w == 2.0f)
        {
            _2488 = max(value, 0.0f.xxx);
        }
        else
        {
            _2488 = value;
        }
        return _2488;
    }
    return 0.0f.xxx;
}

float3 EvaluateSkyBase(float3 dir)
{
    float up = clamp(dir.y, 0.0f, 1.0f);
    float3 horizonCol = float3(4.400000095367431640625f, 4.19999980926513671875f, 3.7999999523162841796875f);
    float3 zenithCol = float3(5.5f, 5.7600002288818359375f, 6.0f);
    return lerp(horizonCol, zenithCol, pow(up, 0.449999988079071044921875f).xxx);
}

float3 sampleAtlasBilinear(float2 uvPx)
{
    float atlasSize = float(volume_SCAtlasSize);
    float2 coord = clamp(uvPx, 0.0f.xx, (atlasSize - 1.0f).xx) - 0.5f.xx;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 f = clamp(frac(coord), 0.0f.xx, 1.0f.xx);
    int maxC = int(atlasSize) - 1;
    float3 c00 = lighting_atlas.Load(int3(clamp(base, int2(0, 0), maxC.xx), 0)).xyz;
    float3 c10 = lighting_atlas.Load(int3(clamp(base + int2(1, 0), int2(0, 0), maxC.xx), 0)).xyz;
    float3 c01 = lighting_atlas.Load(int3(clamp(base + int2(0, 1), int2(0, 0), maxC.xx), 0)).xyz;
    float3 c11 = lighting_atlas.Load(int3(clamp(base + int2(1, 1), int2(0, 0), maxC.xx), 0)).xyz;
    return ((((c00 * (1.0f - f.x)) * (1.0f - f.y)) + ((c10 * f.x) * (1.0f - f.y))) + ((c01 * (1.0f - f.x)) * f.y)) + ((c11 * f.x) * f.y);
}

void comp_main()
{
    uint tid = (gl_LocalInvocationID.y * 8u) + gl_LocalInvocationID.x;
    uint group_id = gl_WorkGroupID.x;
    if (group_id >= volume_ProbeUpdateCount)
    {
        return;
    }
    if (tid >= volume_RaysPerProbe)
    {
        return;
    }
    uint probeIdx = pul.Load(group_id * 4 + 0);
    if (probeIdx >= volume_ProbeCountTotal)
    {
        return;
    }
    uint _3014;
    float3 _3100;
    uint param_11;
    uint param_13;
    bool _3447;
    uint _3517;
    uint _3529;
    uint _3570;
    SurfaceCacheCard card;
    bool accepted;
    float3 scRadiance;
    float3 param_22;
    float param_23;
    uint param_24;
    float3 param_31;
    float param_32;
    uint param_33;
    float3 param_39;
    float param_40;
    float2 candidateUV;
    float2 param_43;
    float _3840;
    float _3844;
    float3 param_47;
    float3 _3939;
    float _4035;
    SurfaceCacheCard card_1;
    float2 param_55;
    for (uint rayId = tid; rayId < volume_RaysPerProbe; rayId += 64u)
    {
        uint globalRayIdx = (group_id * volume_RaysPerProbe) + rayId;
        float hitDistance = asfloat(hd.Load(globalRayIdx * 4 + 0));
        uint3 counts = volume_ProbeCounts.xyz;
        uint param = probeIdx;
        uint3 param_1 = counts;
        uint3 gc = ddgiProbeGridCoord(param, param_1);
        uint3 param_2 = gc;
        float3 param_3 = volume_ProbeOrigin.xyz;
        float param_4 = volume_ProbeSpacing;
        float3 probePos = ddgiProbeWorldPos(param_2, param_3, param_4);
        float3 rayOrigin = probePos;
        bool measuredGeometry = volume_RaySampleMode == 7;
        float4 geometryHN = asfloat(hnb.Load4(globalRayIdx * 16 + 0));
        float4 geometryOrigin = asfloat(robuf.Load4(probeIdx * 16 + 0));
        bool _2959 = geometryOrigin.w >= 0.5f;
        bool _2966;
        if (_2959)
        {
            _2966 = !any(isnan(geometryOrigin));
        }
        else
        {
            _2966 = _2959;
        }
        bool _2973;
        if (_2966)
        {
            _2973 = !any(isinf(geometryOrigin));
        }
        else
        {
            _2973 = _2966;
        }
        bool geometryOriginKnown = _2973;
        bool _2982;
        if (measuredGeometry)
        {
            _2982 = !isnan(geometryHN.w);
        }
        else
        {
            _2982 = measuredGeometry;
        }
        bool _2989;
        if (_2982)
        {
            _2989 = !isinf(geometryHN.w);
        }
        else
        {
            _2989 = _2982;
        }
        bool _2995;
        if (_2989)
        {
            _2995 = geometryHN.w >= 2.0f;
        }
        else
        {
            _2995 = _2989;
        }
        bool _3002;
        if (_2995)
        {
            _3002 = geometryHN.w <= 65537.0f;
        }
        else
        {
            _3002 = _2995;
        }
        bool _3011;
        if (_3002)
        {
            _3011 = floor(geometryHN.w) == geometryHN.w;
        }
        else
        {
            _3011 = _3002;
        }
        bool geometryOwnerTagKnown = _3011;
        if (geometryOwnerTagKnown)
        {
            _3014 = uint(geometryHN.w - 2.0f);
        }
        else
        {
            _3014 = 0u;
        }
        uint geometryOwner = _3014;
        bool _3026 = geometryOwnerTagKnown && geometryOriginKnown;
        bool _3033;
        if (_3026)
        {
            _3033 = volume_SdfVoxelSizes[2].w == 1.0f;
        }
        else
        {
            _3033 = _3026;
        }
        bool _3040;
        if (_3033)
        {
            _3040 = !any(isnan(geometryHN));
        }
        else
        {
            _3040 = _3033;
        }
        bool _3047;
        if (_3040)
        {
            _3047 = !any(isinf(geometryHN));
        }
        else
        {
            _3047 = _3040;
        }
        bool _3056;
        if (_3047)
        {
            _3056 = dot(geometryHN.xyz, geometryHN.xyz) > 0.25f;
        }
        else
        {
            _3056 = _3047;
        }
        bool geometryOwnerKnown = _3056;
        if (measuredGeometry)
        {
            if (geometryOriginKnown)
            {
                rayOrigin = geometryOrigin.xyz;
            }
        }
        else
        {
            if (asfloat(robuf.Load(probeIdx * 16 + 12)) > 0.5f)
            {
                rayOrigin = asfloat(robuf.Load4(probeIdx * 16 + 0)).xyz;
            }
        }
        if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
        {
            _3100 = normalize(-volume_LightDirection.xyz);
        }
        else
        {
            _3100 = float3(0.0f, -1.0f, 0.0f);
        }
        uint param_5 = rayId;
        uint param_6 = volume_RaysPerProbe;
        uint param_7 = volume_FrameIndex;
        uint param_8 = uint(volume_RaySampleMode);
        float3 param_9 = _3100;
        float4 _3121 = ddgiSampleRayDirection(param_5, param_6, param_7, param_8, param_9);
        float3 rayDir = _3121.xyz;
        if (volume_Albedo.w > 0.5f)
        {
            uint sourceBase = ((5u * volume_ProbeCountTotal) * volume_RaysPerProbe) + (4u * globalRayIdx);
            hnb.Store4((sourceBase + 2u) * 16 + 0, asuint(float4(rayOrigin, asfloat(((volume_FrameIndex & 65535u) << 16u) | (probeIdx + 1u)))));
            hnb.Store4((sourceBase + 3u) * 16 + 0, asuint(float4(rayDir, float(rayId + 1u))));
        }
        float4 result = float4(0.0f, 0.0f, 0.0f, -1.0f);
        bool analyticAudit = volume_SdfVoxelSizes[0].w > 0.5f;
        uint auditBase = (volume_ProbeCountTotal * volume_RaysPerProbe) + (4u * globalRayIdx);
        if (analyticAudit)
        {
            hnb.Store4(auditBase * 16 + 0, asuint(0.0f.xxxx));
            hnb.Store4((auditBase + 1u) * 16 + 0, asuint(0.0f.xxxx));
            hnb.Store4((auditBase + 2u) * 16 + 0, asuint(0.0f.xxxx));
        }
        bool querySourceAudit = volume_SdfExtents[2].w > 0.5f;
        uint queryAuditBase = ((9u * volume_ProbeCountTotal) * volume_RaysPerProbe) + (18u * globalRayIdx);
        uint querySelectedCard = 0u;
        float queryBestKey = 1000000015047466219876688855040.0f;
        uint queryCell = 4294967295u;
        if (querySourceAudit)
        {
            float3 queryHeaderPos = rayOrigin;
            uint queryStamp = ((volume_FrameIndex & 65535u) << 16u) | (probeIdx + 1u);
            hnb.Store4(queryAuditBase * 16 + 0, asuint(float4(queryHeaderPos, asfloat(queryStamp))));
            hnb.Store4((queryAuditBase + 1u) * 16 + 0, asuint(float4(1000000015047466219876688855040.0f, 0.0f, asfloat(4294967295u), float(rayId + 1u))));
            for (uint i = 0u; i < 16u; i++)
            {
                hnb.Store4(((queryAuditBase + 2u) + i) * 16 + 0, asuint(float4(0.0f, 1000000015047466219876688855040.0f, 0.0f, asfloat(4294967295u))));
            }
        }
        if (hitDistance >= 0.0f)
        {
            float3 hitPos = rayOrigin + (rayDir * hitDistance);
            if (querySourceAudit)
            {
                uint queryStamp_1 = ((volume_FrameIndex & 65535u) << 16u) | (probeIdx + 1u);
                hnb.Store4(queryAuditBase * 16 + 0, asuint(float4(hitPos, asfloat(queryStamp_1))));
            }
            result.w = hitDistance;
            float3 N = -rayDir;
            float4 hn = asfloat(hnb.Load4(globalRayIdx * 16 + 0));
            bool _3300 = hn.w > 0.5f;
            bool _3309;
            if (_3300)
            {
                _3309 = dot(hn.xyz, hn.xyz) > 0.25f;
            }
            else
            {
                _3309 = _3300;
            }
            if (_3309)
            {
                N = normalize(hn.xyz);
            }
            float3 materialPos = hitPos;
            bool materialProjected = false;
            bool _3324;
            if (analyticAudit)
            {
                _3324 = volume_SdfExtents[0].w > 0.5f;
            }
            else
            {
                _3324 = analyticAudit;
            }
            if (_3324)
            {
                float4 projected = asfloat(hnb.Load4((auditBase + 3u) * 16 + 0));
                if (projected.w > 0.5f)
                {
                    materialPos = projected.xyz;
                    materialProjected = true;
                }
            }
            float3 param_10 = materialPos;
            float3 _3344 = sampleHitAlbedo(param_10, param_11);
            uint albedoSource = param_11;
            float3 albedo = _3344;
            if (materialProjected && (albedoSource != 4u))
            {
                float3 param_12 = hitPos;
                float3 _3355 = sampleHitAlbedo(param_12, param_13);
                albedoSource = param_13;
                albedo = _3355;
            }
            bool scUsed = false;
            uint geometryRejectSummary = 17u;
            bool geometrySawOwnerCandidate = false;
            bool _3368;
            if (measuredGeometry)
            {
                _3368 = volume_SdfVoxelSizes[2].z == 1.0f;
            }
            else
            {
                _3368 = measuredGeometry;
            }
            bool geometryUseOwnerDirectory = _3368;
            uint geometryOwnerBegin = 0u;
            uint geometryOwnerEnd = 0u;
            bool geometryOwnerDirectoryValid = false;
            bool _3374 = geometryUseOwnerDirectory && geometryOwnerKnown;
            bool _3382;
            if (_3374)
            {
                _3382 = geometryOwner < volume_SCLookupCount;
            }
            else
            {
                _3382 = _3374;
            }
            bool _3395;
            if (_3382)
            {
                uint _3391;
                cl.GetDimensions(_3391);
                _3391 = (_3391 - 0) / 48;
                _3395 = geometryOwner < uint(int(_3391));
            }
            else
            {
                _3395 = _3382;
            }
            if (_3395)
            {
                geometryOwnerBegin = cl.Load(geometryOwner * 48 + 40);
                geometryOwnerEnd = cl.Load(geometryOwner * 48 + 44);
                bool _3406 = geometryOwnerBegin <= geometryOwnerEnd;
                bool _3419;
                if (_3406)
                {
                    uint _3415;
                    cd.GetDimensions(_3415);
                    _3415 = (_3415 - 0) / 64;
                    _3419 = geometryOwnerEnd <= uint(int(_3415));
                }
                else
                {
                    _3419 = _3406;
                }
                geometryOwnerDirectoryValid = _3419;
            }
            bool _3421 = !measuredGeometry;
            bool _3468;
            if (!_3421)
            {
                bool _3431;
                if (geometryOwnerKnown)
                {
                    _3431 = volume_SCLookupCount > 0u;
                }
                else
                {
                    _3431 = geometryOwnerKnown;
                }
                bool _3437;
                if (_3431)
                {
                    _3437 = volume_SCAtlasSize > 0u;
                }
                else
                {
                    _3437 = _3431;
                }
                bool _3443;
                if (_3437)
                {
                    uint _3440;
                    cd.GetDimensions(_3440);
                    _3440 = (_3440 - 0) / 64;
                    _3443 = int(_3440) > 0;
                }
                else
                {
                    _3443 = _3437;
                }
                bool _3467;
                if (_3443)
                {
                    if (geometryUseOwnerDirectory)
                    {
                        _3447 = geometryOwnerDirectoryValid;
                    }
                    else
                    {
                        uint _3452;
                        scgo.GetDimensions(_3452);
                        _3452 = (_3452 - 0) / 4;
                        bool _3455 = int(_3452) >= 18;
                        bool _3465;
                        if (_3455)
                        {
                            uint _3462;
                            scgc.GetDimensions(_3462);
                            _3462 = (_3462 - 0) / 4;
                            _3465 = int(_3462) > 0;
                        }
                        else
                        {
                            _3465 = _3455;
                        }
                        _3447 = _3465;
                    }
                    _3467 = _3447;
                }
                else
                {
                    _3467 = _3443;
                }
                _3468 = _3467;
            }
            else
            {
                _3468 = _3421;
            }
            if (_3468)
            {
                if (measuredGeometry)
                {
                    geometryRejectSummary = 16u;
                }
                uint deviceReads = 0u;
                uint cell = 4294967295u;
                if (!geometryUseOwnerDirectory)
                {
                    SCGridParams gp = scGridLoad();
                    SCGridParams param_14 = gp;
                    float3 param_15 = hitPos;
                    cell = scGridCellIndex(param_14, param_15);
                }
                bool _3508;
                if (!geometryUseOwnerDirectory)
                {
                    bool _3492 = cell != 4294967295u;
                    bool _3507;
                    if (_3492)
                    {
                        bool _3496 = !measuredGeometry;
                        bool _3506;
                        if (!_3496)
                        {
                            uint _3501;
                            scgo.GetDimensions(_3501);
                            _3501 = (_3501 - 0) / 4;
                            _3506 = cell < (uint(int(_3501)) - 17u);
                        }
                        else
                        {
                            _3506 = _3496;
                        }
                        _3507 = _3506;
                    }
                    else
                    {
                        _3507 = _3492;
                    }
                    _3508 = _3507;
                }
                else
                {
                    _3508 = geometryUseOwnerDirectory;
                }
                if (_3508)
                {
                    if (querySourceAudit)
                    {
                        queryCell = cell;
                    }
                    if (geometryUseOwnerDirectory)
                    {
                        _3517 = geometryOwnerBegin;
                    }
                    else
                    {
                        _3517 = scgo.Load((16u + cell) * 4 + 0);
                    }
                    uint begin = _3517;
                    if (geometryUseOwnerDirectory)
                    {
                        _3529 = geometryOwnerEnd;
                    }
                    else
                    {
                        _3529 = scgo.Load((17u + cell) * 4 + 0);
                    }
                    uint end = _3529;
                    float bestKey = 1000000015047466219876688855040.0f;
                    for (uint gi = begin; (gi < end) && (deviceReads < 16u); gi++)
                    {
                        bool _3556 = measuredGeometry && (!geometryUseOwnerDirectory);
                        bool _3564;
                        if (_3556)
                        {
                            uint _3560;
                            scgc.GetDimensions(_3560);
                            _3560 = (_3560 - 0) / 4;
                            _3564 = gi >= uint(int(_3560));
                        }
                        else
                        {
                            _3564 = _3556;
                        }
                        if (_3564)
                        {
                            break;
                        }
                        if (geometryUseOwnerDirectory)
                        {
                            _3570 = gi;
                        }
                        else
                        {
                            _3570 = scgc.Load(gi * 4 + 0);
                        }
                        uint cardIdx = _3570;
                        if (cardIdx >= 4096u)
                        {
                            continue;
                        }
                        bool _3593;
                        if (measuredGeometry)
                        {
                            uint _3589;
                            cd.GetDimensions(_3589);
                            _3589 = (_3589 - 0) / 64;
                            _3593 = cardIdx >= uint(int(_3589));
                        }
                        else
                        {
                            _3593 = measuredGeometry;
                        }
                        if (_3593)
                        {
                            continue;
                        }
                        SurfaceCacheCard _3601;
                        _3601.center = asfloat(cd.Load4(cardIdx * 64 + 0));
                        _3601.extent = asfloat(cd.Load4(cardIdx * 64 + 16));
                        _3601.axis_direction = cd.Load(cardIdx * 64 + 32);
                        _3601.resolution = cd.Load(cardIdx * 64 + 36);
                        _3601.atlas_offset_x = cd.Load(cardIdx * 64 + 40);
                        _3601.atlas_offset_y = cd.Load(cardIdx * 64 + 44);
                        _3601.mesh_instance_id = cd.Load(cardIdx * 64 + 48);
                        _3601.static_mesh_slot = cd.Load(cardIdx * 64 + 52);
                        _3601.flags = cd.Load(cardIdx * 64 + 56);
                        _3601.page_card = cd.Load(cardIdx * 64 + 60);
                        card.center = _3601.center;
                        card.extent = _3601.extent;
                        card.axis_direction = _3601.axis_direction;
                        card.resolution = _3601.resolution;
                        card.atlas_offset_x = _3601.atlas_offset_x;
                        card.atlas_offset_y = _3601.atlas_offset_y;
                        card.mesh_instance_id = _3601.mesh_instance_id;
                        card.static_mesh_slot = _3601.static_mesh_slot;
                        card.flags = _3601.flags;
                        card.page_card = _3601.page_card;
                        bool _3635;
                        if (measuredGeometry)
                        {
                            bool _3626 = !geometryOwnerKnown;
                            bool _3634;
                            if (!_3626)
                            {
                                _3634 = card.mesh_instance_id != geometryOwner;
                            }
                            else
                            {
                                _3634 = _3626;
                            }
                            _3635 = _3634;
                        }
                        else
                        {
                            _3635 = measuredGeometry;
                        }
                        if (_3635)
                        {
                            continue;
                        }
                        uint queryCandidateSlot = deviceReads;
                        deviceReads++;
                        float key = 1000000015047466219876688855040.0f;
                        uint reject = 0u;
                        if (measuredGeometry)
                        {
                            SurfaceCacheCard param_16 = card;
                            uint param_17 = geometryOwner;
                            bool param_18 = geometryOwnerKnown;
                            float3 param_19 = hitPos;
                            float3 param_20 = N;
                            float3 param_21 = rayDir;
                            bool _3665 = ddgiMeasuredOwnerCandidate(param_16, param_17, param_18, param_19, param_20, param_21, param_22, param_23, param_24);
                            scRadiance = param_22;
                            key = param_23;
                            reject = param_24;
                            accepted = _3665;
                            if (!geometrySawOwnerCandidate)
                            {
                                geometryRejectSummary = 11u;
                            }
                            geometrySawOwnerCandidate = true;
                            if (!accepted)
                            {
                                geometryRejectSummary = max(geometryRejectSummary, reject);
                            }
                        }
                        else
                        {
                            if (querySourceAudit)
                            {
                                SurfaceCacheCard param_25 = card;
                                float3 param_26 = hitPos;
                                float3 param_27 = -rayDir;
                                float param_28 = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
                                float param_29 = 0.0f;
                                float3 param_30 = 0.0f.xxx;
                                bool _3702 = scQueryCardCandidateDbg(param_25, param_26, param_27, param_28, param_29, depth_atlas, lighting_atlas, param_30, param_31, param_32, param_33);
                                scRadiance = param_31;
                                key = param_32;
                                reject = param_33;
                                accepted = _3702;
                            }
                            else
                            {
                                SurfaceCacheCard param_34 = card;
                                float3 param_35 = hitPos;
                                float3 param_36 = -rayDir;
                                float param_37 = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
                                float3 param_38 = 0.0f.xxx;
                                bool _3722 = scQueryCardCandidate(param_34, param_35, param_36, param_37, depth_atlas, lighting_atlas, param_38, param_39, param_40);
                                scRadiance = param_39;
                                key = param_40;
                                accepted = _3722;
                            }
                        }
                        if (querySourceAudit)
                        {
                            uint packedTexel = 4294967295u;
                            bool _3731 = volume_SCAtlasSize > 0u;
                            bool _3749;
                            if (_3731)
                            {
                                float3 param_41 = hitPos;
                                SurfaceCacheCard param_42 = card;
                                float param_44 = 1.0f / float(max(card.resolution, 1u));
                                float param_45 = 0.0f;
                                bool _3747 = worldToCardUVMarginPad(param_41, param_42, param_43, param_44, param_45);
                                candidateUV = param_43;
                                _3749 = _3747;
                            }
                            else
                            {
                                _3749 = _3731;
                            }
                            bool _3756;
                            if (_3749)
                            {
                                _3756 = !any(isnan(candidateUV));
                            }
                            else
                            {
                                _3756 = _3749;
                            }
                            bool _3763;
                            if (_3756)
                            {
                                _3763 = !any(isinf(candidateUV));
                            }
                            else
                            {
                                _3763 = _3756;
                            }
                            if (_3763)
                            {
                                uint2 texel = uint2(clamp(candidateUV, 0.0f.xx, float2(float((card.atlas_offset_x + card.resolution) - 1u), float((card.atlas_offset_y + card.resolution) - 1u))));
                                packedTexel = (texel.y * volume_SCAtlasSize) + texel.x;
                            }
                            hnb.Store4(((queryAuditBase + 2u) + queryCandidateSlot) * 16 + 0, asuint(float4(float(cardIdx + 1u), key, float(reject), asfloat(packedTexel))));
                        }
                        if (accepted)
                        {
                            if (key < bestKey)
                            {
                                bestKey = key;
                                result.x = scRadiance.x;
                                result.y = scRadiance.y;
                                result.z = scRadiance.z;
                                scUsed = true;
                                if (querySourceAudit)
                                {
                                    queryBestKey = key;
                                    querySelectedCard = cardIdx + 1u;
                                }
                            }
                        }
                    }
                }
            }
            if (measuredGeometry)
            {
                float ownerTag = float(geometryOwner + 2u);
                if (geometryOwnerTagKnown)
                {
                    if (scUsed)
                    {
                        _3844 = ownerTag;
                    }
                    else
                    {
                        _3844 = -(ownerTag + (float(geometryRejectSummary) / 32.0f));
                    }
                    _3840 = _3844;
                }
                else
                {
                    _3840 = -1.0f;
                }
                hnb.Store(globalRayIdx * 16 + 12, asuint(_3840));
            }
            if (querySourceAudit)
            {
                hnb.Store4((queryAuditBase + 1u) * 16 + 0, asuint(float4(queryBestKey, float(querySelectedCard), asfloat(queryCell), float(rayId + 1u))));
            }
            if ((!scUsed) && (!measuredGeometry))
            {
                float3 param_46 = hitPos;
                bool _3886 = samplePrevColor(param_46, param_47);
                float3 prevRadiance = param_47;
                if (_3886)
                {
                    float prevLum = dot(prevRadiance, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
                    prevRadiance *= (1.0f / (1.0f + (max(prevLum - 1.0f, 0.0f) * 2.0f)));
                    result.x = prevRadiance.x;
                    result.y = prevRadiance.y;
                    result.z = prevRadiance.z;
                }
                else
                {
                    float3 historyPos = hitPos;
                    float3 param_48 = historyPos;
                    float3 param_49 = N;
                    float3 L_i_prev = sampleDiffuseHistoryIrradianceOverPi(param_48, param_49);
                    float NdotUp = max(0.0f, N.y);
                    float3 E_sky = (volume_SkyColor.xyz * (0.5f + (0.5f * NdotUp))) * 0.300000011920928955078125f;
                    if (volume_SkyColor.w == 1.0f)
                    {
                        _3939 = 0.0f.xxx;
                    }
                    else
                    {
                        _3939 = L_i_prev * 3.1415927410125732421875f;
                    }
                    float3 historyBounce = _3939;
                    float3 _3952 = (albedo / 3.1415927410125732421875f.xxx) * (E_sky + historyBounce);
                    result.x = _3952.x;
                    result.y = _3952.y;
                    result.z = _3952.z;
                    if (analyticAudit)
                    {
                        hnb.Store4(auditBase * 16 + 0, asuint(float4(albedo, float(albedoSource))));
                        hnb.Store4((auditBase + 1u) * 16 + 0, asuint(float4((albedo / 3.1415927410125732421875f.xxx) * E_sky, 1.0f)));
                        hnb.Store4((auditBase + 2u) * 16 + 0, asuint(float4((albedo / 3.1415927410125732421875f.xxx) * historyBounce, 1.0f)));
                    }
                }
            }
        }
        else
        {
            if (measuredGeometry && (hitDistance == (-3.0f)))
            {
                result = float4(0.0f, 0.0f, 0.0f, -3.0f);
            }
            else
            {
                if (measuredGeometry && (hitDistance != (-2.0f)))
                {
                    bool _4013 = isnan(hitDistance);
                    bool _4019;
                    if (!_4013)
                    {
                        _4019 = isinf(hitDistance);
                    }
                    else
                    {
                        _4019 = _4013;
                    }
                    result = float4(0.0f, 0.0f, 0.0f, _4019 ? (-1.0f) : hitDistance);
                }
                else
                {
                    if (hitDistance <= (-1.5f))
                    {
                        result.w = hitDistance;
                        if (rayDir.y < 0.0f)
                        {
                            _4035 = 1.0f - clamp(volume_SdfOrigins[0].w, 0.0f, 1.0f);
                        }
                        else
                        {
                            _4035 = 1.0f;
                        }
                        float lowerEnvScale = _4035;
                        float3 param_50 = rayDir;
                        float3 _4053 = (EvaluateSkyBase(param_50) * volume_SkyColor.xyz) * lowerEnvScale;
                        result.x = _4053.x;
                        result.y = _4053.y;
                        result.z = _4053.z;
                    }
                    else
                    {
                        uint deviceReads_1 = 0u;
                        bool found = false;
                        SCGridParams gp_1 = scGridLoad();
                        SCGridParams param_51 = gp_1;
                        float3 param_52 = rayOrigin;
                        uint cell_1 = scGridCellIndex(param_51, param_52);
                        if (cell_1 != 4294967295u)
                        {
                            uint begin_1 = scgo.Load((16u + cell_1) * 4 + 0);
                            uint end_1 = scgo.Load((17u + cell_1) * 4 + 0);
                            for (uint gi_1 = begin_1; (gi_1 < end_1) && (deviceReads_1 < 8u); gi_1++)
                            {
                                uint cardIdx_1 = scgc.Load(gi_1 * 4 + 0);
                                if (cardIdx_1 >= 4096u)
                                {
                                    continue;
                                }
                                SurfaceCacheCard _4110;
                                _4110.center = asfloat(cd.Load4(cardIdx_1 * 64 + 0));
                                _4110.extent = asfloat(cd.Load4(cardIdx_1 * 64 + 16));
                                _4110.axis_direction = cd.Load(cardIdx_1 * 64 + 32);
                                _4110.resolution = cd.Load(cardIdx_1 * 64 + 36);
                                _4110.atlas_offset_x = cd.Load(cardIdx_1 * 64 + 40);
                                _4110.atlas_offset_y = cd.Load(cardIdx_1 * 64 + 44);
                                _4110.mesh_instance_id = cd.Load(cardIdx_1 * 64 + 48);
                                _4110.static_mesh_slot = cd.Load(cardIdx_1 * 64 + 52);
                                _4110.flags = cd.Load(cardIdx_1 * 64 + 56);
                                _4110.page_card = cd.Load(cardIdx_1 * 64 + 60);
                                card_1.center = _4110.center;
                                card_1.extent = _4110.extent;
                                card_1.axis_direction = _4110.axis_direction;
                                card_1.resolution = _4110.resolution;
                                card_1.atlas_offset_x = _4110.atlas_offset_x;
                                card_1.atlas_offset_y = _4110.atlas_offset_y;
                                card_1.mesh_instance_id = _4110.mesh_instance_id;
                                card_1.static_mesh_slot = _4110.static_mesh_slot;
                                card_1.flags = _4110.flags;
                                card_1.page_card = _4110.page_card;
                                deviceReads_1++;
                                float3 param_53 = rayOrigin;
                                SurfaceCacheCard param_54 = card_1;
                                bool _4139 = worldToCardUV(param_53, param_54, param_55);
                                float2 atlasUV = param_55;
                                if (_4139)
                                {
                                    float2 param_56 = atlasUV;
                                    float3 missRadiance = sampleAtlasBilinear(param_56);
                                    if (dot(missRadiance, missRadiance) > 9.9999997473787516355514526367188e-05f)
                                    {
                                        result.x = missRadiance.x;
                                        result.y = missRadiance.y;
                                        result.z = missRadiance.z;
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (!found)
                        {
                            result.x = 0.0f;
                            result.y = 0.0f;
                            result.z = 0.0f;
                        }
                    }
                }
            }
        }
        rb.Store4(((probeIdx * volume_RaysPerProbe) + rayId) * 16 + 0, asuint(result));
    }
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationID = stage_input.gl_LocalInvocationID;
    comp_main();
}
