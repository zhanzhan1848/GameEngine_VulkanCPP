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

static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

struct SurfaceCacheCardLookup
{
    float4 aabb_min;
    float4 aabb_max;
    uint card_start;
    uint card_count;
    uint _pad0;
    uint _pad1;
};

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
ByteAddressBuffer robuf : register(t17, space0);
RWByteAddressBuffer hnb : register(u14, space0);
ByteAddressBuffer scgc : register(t12, space0);
ByteAddressBuffer cd : register(t6, space0);
RWByteAddressBuffer rb : register(u4, space0);
ByteAddressBuffer cl : register(t5, space0);
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
    uint _1147 = ddgiOctSampleHash(param);
    uint param_1 = rayIndex ^ 2246822507u;
    uint _1152 = ddgiOctSampleHash(param_1);
    uint seed = _1147 ^ _1152;
    uint param_2 = seed ^ 1757159915u;
    uint _1159 = ddgiOctSampleHash(param_2);
    uint param_3 = seed ^ 48610963u;
    uint _1167 = ddgiOctSampleHash(param_3);
    float2 jitter = (float2(float(_1159 >> 14u), float(_1167 >> 14u)) + 0.5f.xx) / 262144.0f.xx;
    float2 uv = (float2(float(cell % 8u), float(cell / 8u)) + jitter) / 8.0f.xx;
    float2 oct = (uv * 2.0f) - 1.0f.xx;
    float3 q = float3(oct, (1.0f - abs(oct.x)) - abs(oct.y));
    if (q.z < 0.0f)
    {
        float3 _1212 = q;
        float3 _1219 = q;
        bool2 _1222 = bool2(_1219.xy.x >= 0.0f.xx.x, _1219.xy.y >= 0.0f.xx.y);
        float2 _1224 = (1.0f.xx - abs(_1212.yx)) * float2(_1222.x ? 1.0f.xx.x : (-1.0f).xx.x, _1222.y ? 1.0f.xx.y : (-1.0f).xx.y);
        q.x = _1224.x;
        q.y = _1224.y;
    }
    float qLength = length(q);
    return float4(q / qLength.xxx, ((qLength * qLength) * qLength) * 0.25f);
}

float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex, inout uint mode, float3 toLight)
{
    if (mode == 7u)
    {
        bool _1253 = rayCount >= 64u;
        bool _1259;
        if (_1253)
        {
            _1259 = (rayCount % 64u) == 0u;
        }
        else
        {
            _1259 = _1253;
        }
        if (_1259)
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
    bool _1751 = any(bool3(ndc.x < float3(-1.0f, -1.0f, 0.0f).x, ndc.y < float3(-1.0f, -1.0f, 0.0f).y, ndc.z < float3(-1.0f, -1.0f, 0.0f).z));
    bool _1759;
    if (!_1751)
    {
        _1759 = any(bool3(ndc.x > 1.0f.xxx.x, ndc.y > 1.0f.xxx.y, ndc.z > 1.0f.xxx.z));
    }
    else
    {
        _1759 = _1751;
    }
    if (_1759)
    {
        return false;
    }
    uint _1765_dummy_parameter;
    int2 dims = int2(spvTextureSize(depthTexture, uint(0), _1765_dummy_parameter));
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
    bool _1927 = projectVisibleHit(param, param_1, param_2, screen_depth, param_3, param_4);
    int2 texel = param_3;
    source = param_4;
    if (!_1927)
    {
        return volume_Albedo.xyz;
    }
    source = 3u;
    uint _1939_dummy_parameter;
    int2 _1939 = int2(spvTextureSize(gbuffer_albedo, uint(0), _1939_dummy_parameter));
    uint _1941_dummy_parameter;
    int2 _1941 = int2(spvTextureSize(screen_depth, uint(0), _1941_dummy_parameter));
    if (any(bool2(_1939.x != _1941.x, _1939.y != _1941.y)))
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
    bool _860 = g.x < 0.0f;
    bool _867;
    if (!_860)
    {
        _867 = g.y < 0.0f;
    }
    else
    {
        _867 = _860;
    }
    bool _874;
    if (!_867)
    {
        _874 = g.z < 0.0f;
    }
    else
    {
        _874 = _867;
    }
    if (_874)
    {
        return 4294967295u;
    }
    uint3 c = uint3(g);
    bool _886 = c.x >= gp.dims.x;
    bool _895;
    if (!_886)
    {
        _895 = c.y >= gp.dims.y;
    }
    else
    {
        _895 = _886;
    }
    bool _904;
    if (!_895)
    {
        _904 = c.z >= gp.dims.z;
    }
    else
    {
        _904 = _895;
    }
    if (_904)
    {
        return 4294967295u;
    }
    return c.x + (gp.dims.x * (c.y + (gp.dims.y * c.z)));
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
        float _421;
        if (axis == 0u)
        {
            _421 = card.extent.y;
        }
        else
        {
            float _429;
            if (axis == 1u)
            {
                _429 = card.extent.x;
            }
            else
            {
                _429 = card.extent.x;
            }
            _421 = _429;
        }
        float eu = _421;
        float _442;
        if (axis == 0u)
        {
            _442 = card.extent.z;
        }
        else
        {
            float _450;
            if (axis == 1u)
            {
                _450 = card.extent.z;
            }
            else
            {
                _450 = card.extent.y;
            }
            _442 = _450;
        }
        float ev = _442;
        float _463;
        if (eu > 9.9999997473787516355514526367188e-05f)
        {
            _463 = pad_m / (eu * 2.0f);
        }
        else
        {
            _463 = 0.0f;
        }
        padU = _463;
        float _474;
        if (ev > 9.9999997473787516355514526367188e-05f)
        {
            _474 = pad_m / (ev * 2.0f);
        }
        else
        {
            _474 = 0.0f;
        }
        padV = _474;
    }
    bool _488 = u < ((-margin_uv) - padU);
    bool _498;
    if (!_488)
    {
        _498 = u >= ((1.0f + margin_uv) + padU);
    }
    else
    {
        _498 = _488;
    }
    bool _508;
    if (!_498)
    {
        _508 = v < ((-margin_uv) - padV);
    }
    else
    {
        _508 = _498;
    }
    bool _518;
    if (!_508)
    {
        _518 = v >= ((1.0f + margin_uv) + padV);
    }
    else
    {
        _518 = _508;
    }
    if (_518)
    {
        return false;
    }
    u = clamp(u, 0.0f, 0.999998986721038818359375f);
    v = clamp(v, 0.0f, 0.999998986721038818359375f);
    out_uv.x = float(card.atlas_offset_x) + (u * float(card.resolution));
    out_uv.y = float(card.atlas_offset_y) + (v * float(card.resolution));
    return true;
}

float3 scCardFaceNormal(SurfaceCacheCard card)
{
    uint axis = card.axis_direction & 255u;
    uint dir = (card.axis_direction >> 8u) & 255u;
    float s = (dir > 0u) ? 1.0f : (-1.0f);
    float3 _588;
    if (axis == 0u)
    {
        _588 = float3(s, 0.0f, 0.0f);
    }
    else
    {
        float3 _596;
        if (axis == 1u)
        {
            _596 = float3(0.0f, s, 0.0f);
        }
        else
        {
            _596 = float3(0.0f, 0.0f, s);
        }
        _588 = _596;
    }
    return _588;
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
    bool _624 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    float2 atlasUV = param_2;
    if (!_624)
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
    bool _689 = isnan(depth);
    bool _695;
    if (!_689)
    {
        _695 = isinf(depth);
    }
    else
    {
        _695 = _689;
    }
    if (_695 || (depth < 0.0f))
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
    bool _775 = any(isnan(c));
    bool _782;
    if (!_775)
    {
        _782 = any(isinf(c));
    }
    else
    {
        _782 = _775;
    }
    if (_782)
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
    bool _821 = scQueryCardCandidateDbg(param, param_1, param_2, param_3, param_4, depthAtlas, lightingAtlas, param_5, param_6, param_7, param_8);
    outRadiance = param_6;
    outKey = param_7;
    uint rejectDummy = param_8;
    return _821;
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
    bool _1866 = projectVisibleHit(param, param_1, param_2, radiance_depth, param_3, param_4);
    int2 texel = param_3;
    uint reject = param_4;
    if (!_1866)
    {
        return false;
    }
    uint _1875_dummy_parameter;
    int2 _1875 = int2(spvTextureSize(prev_frame_color, uint(0), _1875_dummy_parameter));
    uint _1877_dummy_parameter;
    int2 _1877 = int2(spvTextureSize(radiance_depth, uint(0), _1877_dummy_parameter));
    if (any(bool2(_1875.x != _1877.x, _1875.y != _1877.y)))
    {
        return false;
    }
    radiance = prev_frame_color.Load(int3(texel, 0)).xyz;
    bool _1889 = any(isnan(radiance));
    bool _1896;
    if (!_1889)
    {
        _1896 = any(isinf(radiance));
    }
    else
    {
        _1896 = _1889;
    }
    if (_1896)
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
    float3 _1015 = ((((((((sh[0] * 0.282094776630401611328125f) + (sh[1] * ((-0.48860251903533935546875f) * d.y))) + (sh[2] * (0.48860251903533935546875f * d.z))) + (sh[3] * ((-0.48860251903533935546875f) * d.x))) + (sh[4] * ((1.09254848957061767578125f * d.y) * d.x))) + (sh[5] * (((-1.09254848957061767578125f) * d.y) * d.z))) + (sh[6] * (0.3153915703296661376953125f * (((3.0f * d.z) * d.z) - 1.0f)))) + (sh[7] * (((-1.09254848957061767578125f) * d.x) * d.z))) + (sh[8] * (0.5462741851806640625f * ((d.x * d.x) - (d.y * d.y))));
    return _1015;
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
    bool _2365 = any(bool3(gp.x < 0.0f.xxx.x, gp.y < 0.0f.xxx.y, gp.z < 0.0f.xxx.z));
    bool _2373;
    if (!_2365)
    {
        _2373 = any(bool3(gp.x > gridMax.x, gp.y > gridMax.y, gp.z > gridMax.z));
    }
    else
    {
        _2373 = _2365;
    }
    if (_2373)
    {
        return 0.0f.xxx;
    }
    float3 param = gp;
    uint3 param_1 = counts;
    TetraResult tet = tetrahedral(param, param_1);
    float3 result = 0.0f.xxx;
    float totalWeight = 0.0f;
    float3 sh[9];
    float3 _2434;
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
            _2434 = evalProbeIrradianceSH9(param_2, param_3);
        }
        else
        {
            float3 param_4[9] = sh;
            float3 param_5 = N;
            _2434 = shDot9(param_4, param_5);
        }
        float3 irradiance = _2434;
        result += (irradiance * tet.bw[p]);
        totalWeight += tet.bw[p];
    }
    if (totalWeight > 0.0f)
    {
        float3 value = result / totalWeight.xxx;
        float3 _2475;
        if (volume_SkyColor.w == 2.0f)
        {
            _2475 = max(value, 0.0f.xxx);
        }
        else
        {
            _2475 = value;
        }
        return _2475;
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

bool worldToCardUVMargin(float3 world_pos, SurfaceCacheCard card, out float2 out_uv, float margin_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = margin_uv;
    float param_4 = 0.0f;
    float2 param_2;
    bool _558 = worldToCardUVMarginPad(param, param_1, param_2, param_3, param_4);
    out_uv = param_2;
    return _558;
}

bool worldToCardUV(float3 world_pos, SurfaceCacheCard card, out float2 out_uv)
{
    float3 param = world_pos;
    SurfaceCacheCard param_1 = card;
    float param_3 = 0.0f;
    float2 param_2;
    bool _568 = worldToCardUVMargin(param, param_1, param_2, param_3);
    out_uv = param_2;
    return _568;
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
    float3 _2619;
    uint param_11;
    uint param_13;
    SurfaceCacheCard card;
    bool accepted;
    float3 scRadiance;
    float3 param_22;
    float param_23;
    uint param_24;
    float3 param_30;
    float param_31;
    float2 candidateUV;
    float2 param_34;
    float3 param_38;
    float3 _3204;
    float _3272;
    SurfaceCacheCard card_1;
    float2 param_46;
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
        if (asfloat(robuf.Load(probeIdx * 16 + 12)) > 0.5f)
        {
            rayOrigin = asfloat(robuf.Load4(probeIdx * 16 + 0)).xyz;
        }
        if ((((volume_LightDirection.x * volume_LightDirection.x) + (volume_LightDirection.y * volume_LightDirection.y)) + (volume_LightDirection.z * volume_LightDirection.z)) > 9.9999999392252902907785028219223e-09f)
        {
            _2619 = normalize(-volume_LightDirection.xyz);
        }
        else
        {
            _2619 = float3(0.0f, -1.0f, 0.0f);
        }
        uint param_5 = rayId;
        uint param_6 = volume_RaysPerProbe;
        uint param_7 = volume_FrameIndex;
        uint param_8 = uint(volume_RaySampleMode);
        float3 param_9 = _2619;
        float4 _2640 = ddgiSampleRayDirection(param_5, param_6, param_7, param_8, param_9);
        float3 rayDir = _2640.xyz;
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
            bool _2825 = hn.w > 0.5f;
            bool _2834;
            if (_2825)
            {
                _2834 = dot(hn.xyz, hn.xyz) > 0.25f;
            }
            else
            {
                _2834 = _2825;
            }
            if (_2834)
            {
                N = normalize(hn.xyz);
            }
            float3 materialPos = hitPos;
            bool materialProjected = false;
            bool _2849;
            if (analyticAudit)
            {
                _2849 = volume_SdfExtents[0].w > 0.5f;
            }
            else
            {
                _2849 = analyticAudit;
            }
            if (_2849)
            {
                float4 projected = asfloat(hnb.Load4((auditBase + 3u) * 16 + 0));
                if (projected.w > 0.5f)
                {
                    materialPos = projected.xyz;
                    materialProjected = true;
                }
            }
            float3 param_10 = materialPos;
            float3 _2869 = sampleHitAlbedo(param_10, param_11);
            uint albedoSource = param_11;
            float3 albedo = _2869;
            if (materialProjected && (albedoSource != 4u))
            {
                float3 param_12 = hitPos;
                float3 _2880 = sampleHitAlbedo(param_12, param_13);
                albedoSource = param_13;
                albedo = _2880;
            }
            bool scUsed = false;
            uint deviceReads = 0u;
            SCGridParams gp = scGridLoad();
            SCGridParams param_14 = gp;
            float3 param_15 = hitPos;
            uint cell = scGridCellIndex(param_14, param_15);
            if (cell != 4294967295u)
            {
                if (querySourceAudit)
                {
                    queryCell = cell;
                }
                uint begin = scgo.Load((16u + cell) * 4 + 0);
                uint end = scgo.Load((17u + cell) * 4 + 0);
                float bestKey = 1000000015047466219876688855040.0f;
                for (uint gi = begin; (gi < end) && (deviceReads < 16u); gi++)
                {
                    uint cardIdx = scgc.Load(gi * 4 + 0);
                    if (cardIdx >= 4096u)
                    {
                        continue;
                    }
                    SurfaceCacheCard _2948;
                    _2948.center = asfloat(cd.Load4(cardIdx * 64 + 0));
                    _2948.extent = asfloat(cd.Load4(cardIdx * 64 + 16));
                    _2948.axis_direction = cd.Load(cardIdx * 64 + 32);
                    _2948.resolution = cd.Load(cardIdx * 64 + 36);
                    _2948.atlas_offset_x = cd.Load(cardIdx * 64 + 40);
                    _2948.atlas_offset_y = cd.Load(cardIdx * 64 + 44);
                    _2948.mesh_instance_id = cd.Load(cardIdx * 64 + 48);
                    _2948.static_mesh_slot = cd.Load(cardIdx * 64 + 52);
                    _2948.flags = cd.Load(cardIdx * 64 + 56);
                    _2948.page_card = cd.Load(cardIdx * 64 + 60);
                    card.center = _2948.center;
                    card.extent = _2948.extent;
                    card.axis_direction = _2948.axis_direction;
                    card.resolution = _2948.resolution;
                    card.atlas_offset_x = _2948.atlas_offset_x;
                    card.atlas_offset_y = _2948.atlas_offset_y;
                    card.mesh_instance_id = _2948.mesh_instance_id;
                    card.static_mesh_slot = _2948.static_mesh_slot;
                    card.flags = _2948.flags;
                    card.page_card = _2948.page_card;
                    uint queryCandidateSlot = deviceReads;
                    deviceReads++;
                    float key = 1000000015047466219876688855040.0f;
                    uint reject = 0u;
                    if (querySourceAudit)
                    {
                        SurfaceCacheCard param_16 = card;
                        float3 param_17 = hitPos;
                        float3 param_18 = -rayDir;
                        float param_19 = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
                        float param_20 = 0.0f;
                        float3 param_21 = 0.0f.xxx;
                        bool _2999 = scQueryCardCandidateDbg(param_16, param_17, param_18, param_19, param_20, depth_atlas, lighting_atlas, param_21, param_22, param_23, param_24);
                        scRadiance = param_22;
                        key = param_23;
                        reject = param_24;
                        accepted = _2999;
                    }
                    else
                    {
                        SurfaceCacheCard param_25 = card;
                        float3 param_26 = hitPos;
                        float3 param_27 = -rayDir;
                        float param_28 = max(volume_SdfVoxelSizes[0].x * 1.25f, 0.449999988079071044921875f);
                        float3 param_29 = 0.0f.xxx;
                        bool _3019 = scQueryCardCandidate(param_25, param_26, param_27, param_28, depth_atlas, lighting_atlas, param_29, param_30, param_31);
                        scRadiance = param_30;
                        key = param_31;
                        accepted = _3019;
                    }
                    if (querySourceAudit)
                    {
                        uint packedTexel = 4294967295u;
                        bool _3028 = volume_SCAtlasSize > 0u;
                        bool _3046;
                        if (_3028)
                        {
                            float3 param_32 = hitPos;
                            SurfaceCacheCard param_33 = card;
                            float param_35 = 1.0f / float(max(card.resolution, 1u));
                            float param_36 = 0.0f;
                            bool _3044 = worldToCardUVMarginPad(param_32, param_33, param_34, param_35, param_36);
                            candidateUV = param_34;
                            _3046 = _3044;
                        }
                        else
                        {
                            _3046 = _3028;
                        }
                        bool _3053;
                        if (_3046)
                        {
                            _3053 = !any(isnan(candidateUV));
                        }
                        else
                        {
                            _3053 = _3046;
                        }
                        bool _3060;
                        if (_3053)
                        {
                            _3060 = !any(isinf(candidateUV));
                        }
                        else
                        {
                            _3060 = _3053;
                        }
                        if (_3060)
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
            if (querySourceAudit)
            {
                hnb.Store4((queryAuditBase + 1u) * 16 + 0, asuint(float4(queryBestKey, float(querySelectedCard), asfloat(queryCell), float(rayId + 1u))));
            }
            if (!scUsed)
            {
                float3 param_37 = hitPos;
                bool _3151 = samplePrevColor(param_37, param_38);
                float3 prevRadiance = param_38;
                if (_3151)
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
                    float3 param_39 = historyPos;
                    float3 param_40 = N;
                    float3 L_i_prev = sampleDiffuseHistoryIrradianceOverPi(param_39, param_40);
                    float NdotUp = max(0.0f, N.y);
                    float3 E_sky = (volume_SkyColor.xyz * (0.5f + (0.5f * NdotUp))) * 0.300000011920928955078125f;
                    if (volume_SkyColor.w == 1.0f)
                    {
                        _3204 = 0.0f.xxx;
                    }
                    else
                    {
                        _3204 = L_i_prev * 3.1415927410125732421875f;
                    }
                    float3 historyBounce = _3204;
                    float3 _3217 = (albedo / 3.1415927410125732421875f.xxx) * (E_sky + historyBounce);
                    result.x = _3217.x;
                    result.y = _3217.y;
                    result.z = _3217.z;
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
            if (hitDistance <= (-1.5f))
            {
                result.w = hitDistance;
                if (rayDir.y < 0.0f)
                {
                    _3272 = 1.0f - clamp(volume_SdfOrigins[0].w, 0.0f, 1.0f);
                }
                else
                {
                    _3272 = 1.0f;
                }
                float lowerEnvScale = _3272;
                float3 param_41 = rayDir;
                float3 _3290 = (EvaluateSkyBase(param_41) * volume_SkyColor.xyz) * lowerEnvScale;
                result.x = _3290.x;
                result.y = _3290.y;
                result.z = _3290.z;
            }
            else
            {
                uint deviceReads_1 = 0u;
                bool found = false;
                SCGridParams gp_1 = scGridLoad();
                SCGridParams param_42 = gp_1;
                float3 param_43 = rayOrigin;
                uint cell_1 = scGridCellIndex(param_42, param_43);
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
                        SurfaceCacheCard _3347;
                        _3347.center = asfloat(cd.Load4(cardIdx_1 * 64 + 0));
                        _3347.extent = asfloat(cd.Load4(cardIdx_1 * 64 + 16));
                        _3347.axis_direction = cd.Load(cardIdx_1 * 64 + 32);
                        _3347.resolution = cd.Load(cardIdx_1 * 64 + 36);
                        _3347.atlas_offset_x = cd.Load(cardIdx_1 * 64 + 40);
                        _3347.atlas_offset_y = cd.Load(cardIdx_1 * 64 + 44);
                        _3347.mesh_instance_id = cd.Load(cardIdx_1 * 64 + 48);
                        _3347.static_mesh_slot = cd.Load(cardIdx_1 * 64 + 52);
                        _3347.flags = cd.Load(cardIdx_1 * 64 + 56);
                        _3347.page_card = cd.Load(cardIdx_1 * 64 + 60);
                        card_1.center = _3347.center;
                        card_1.extent = _3347.extent;
                        card_1.axis_direction = _3347.axis_direction;
                        card_1.resolution = _3347.resolution;
                        card_1.atlas_offset_x = _3347.atlas_offset_x;
                        card_1.atlas_offset_y = _3347.atlas_offset_y;
                        card_1.mesh_instance_id = _3347.mesh_instance_id;
                        card_1.static_mesh_slot = _3347.static_mesh_slot;
                        card_1.flags = _3347.flags;
                        card_1.page_card = _3347.page_card;
                        deviceReads_1++;
                        float3 param_44 = rayOrigin;
                        SurfaceCacheCard param_45 = card_1;
                        bool _3376 = worldToCardUV(param_44, param_45, param_46);
                        float2 atlasUV = param_46;
                        if (_3376)
                        {
                            float2 param_47 = atlasUV;
                            float3 missRadiance = sampleAtlasBilinear(param_47);
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
