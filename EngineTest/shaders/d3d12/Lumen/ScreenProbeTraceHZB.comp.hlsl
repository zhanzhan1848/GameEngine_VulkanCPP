static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer GlobalData : register(b4, space0)
{
    row_major float4x4 global_view_projection : packoffset(c0);
    row_major float4x4 global_inv_view_projection : packoffset(c4);
    float4 global_camera_position : packoffset(c8);
    float4 global_grid_params : packoffset(c9);
    float4 global_trace_params : packoffset(c10);
    float4 global_sdf_origins[3] : packoffset(c11);
    float4 global_sdf_voxel_sizes[3] : packoffset(c14);
    float4 global_sdf_extents[3] : packoffset(c17);
    float4 global_sdf_resolutions : packoffset(c20);
    float4 global_surface_cache_params : packoffset(c21);
    float4 global_ddgi_fb[2] : packoffset(c22);
    float4 global_hzb_params : packoffset(c24);
    float4 global_cache_params : packoffset(c25);
    float4 global_diag_params : packoffset(c26);
    float4 global_hint_params : packoffset(c27);
    float4 global_sky_params : packoffset(c28);
    float4 global_wrc_fb[2] : packoffset(c29);
    float4 global_repr_params : packoffset(c31);
    float4 global_tier_params : packoffset(c32);
    float4 global_tier_counts : packoffset(c33);
};

ByteAddressBuffer compactList : register(t6, space0);
ByteAddressBuffer probeNormals : register(t2, space0);
RWByteAddressBuffer hit_buffer : register(u3, space0);
ByteAddressBuffer probePositions : register(t1, space0);
ByteAddressBuffer importanceSH : register(t5, space0);
Texture2D<float4> hzb_texture : register(t0, space0);

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

uint spgiJitterHash(inout uint v)
{
    v ^= (v >> 16u);
    v *= 2146121005u;
    v ^= (v >> 15u);
    v *= 2221713035u;
    v ^= (v >> 16u);
    return v;
}

float3x3 spgiJitterRotation(uint probeIdx, float frameSeed)
{
    if (frameSeed < 0.0f)
    {
        return float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), float3(0.0f, 0.0f, 1.0f));
    }
    uint fs = uint(frameSeed);
    uint param = (probeIdx * 2654435769u) ^ fs;
    uint _168 = spgiJitterHash(param);
    uint h1 = _168;
    uint param_1 = h1 ^ 1757159915u;
    uint _174 = spgiJitterHash(param_1);
    uint h2 = _174;
    float a1 = float(h1) * 1.4629181199765639576071407645941e-09f;
    float a2 = float(h2) * 1.4629181199765639576071407645941e-09f;
    float c1 = cos(a1);
    float s1 = sin(a1);
    float c2 = cos(a2);
    float s2 = sin(a2);
    float3x3 rz1 = float3x3(float3(c1, s1, 0.0f), float3(-s1, c1, 0.0f), float3(0.0f, 0.0f, 1.0f));
    float3x3 rxf = float3x3(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, -1.0f), float3(0.0f, 1.0f, 0.0f));
    float3x3 rz2 = float3x3(float3(c2, s2, 0.0f), float3(-s2, c2, 0.0f), float3(0.0f, 0.0f, 1.0f));
    return mul(rz1, mul(rxf, rz2));
}

float3 fibonacciDirection(uint rayIndex, uint totalRays)
{
    float theta = (6.283185482025146484375f * float(rayIndex)) / 1.61803400516510009765625f;
    float phi = acos(1.0f - ((2.0f * (float(rayIndex) + 0.5f)) / float(totalRays)));
    return float3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

float3 spgiImportanceDirection(uint rayIdx, uint raysPerProbe, uint probeIdx, float frameSeed, float3 probeNormal, float3 sh0, float3 sh1, float3 sh2, float3 sh3, inout float invNq)
{
    uint n = min(raysPerProbe, 64u);
    uint param = probeIdx;
    float param_1 = frameSeed;
    float3x3 rot = spgiJitterRotation(param, param_1);
    bool3 _243 = (abs(probeNormal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_243.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _243.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _243.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, probeNormal));
    float3 bitangent = cross(probeNormal, tangent);
    float sum = 0.0f;
    float pdf[64];
    for (uint i = 0u; i < n; i++)
    {
        uint param_2 = i;
        uint param_3 = n;
        float3 local = mul(fibonacciDirection(param_2, param_3), rot);
        float3 world = ((tangent * local.x) + (bitangent * local.y)) + (probeNormal * local.z);
        float3 irr = (((sh0 * 0.2820949852466583251953125f) + (sh1 * ((-0.48860299587249755859375f) * world.y))) + (sh2 * (0.48860299587249755859375f * world.z))) + (sh3 * ((-0.48860299587249755859375f) * world.x));
        float lum = max(dot(irr, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f)), 0.0f);
        pdf[i] = max(local.z, 0.0199999995529651641845703125f) * (lum + 0.0500000007450580596923828125f);
        sum += pdf[i];
    }
    for (uint i_1 = 0u; i_1 < n; i_1++)
    {
        pdf[i_1] = (0.25f / float(n)) + ((0.75f * pdf[i_1]) / max(sum, 9.9999999747524270787835121154785e-07f));
    }
    sum = 1.0f;
    uint _371;
    if (frameSeed < 0.0f)
    {
        _371 = 0u;
    }
    else
    {
        _371 = uint(frameSeed);
    }
    uint sampleSeed = _371;
    uint param_4 = ((probeIdx * 2654435769u) ^ sampleSeed) ^ (rayIdx * 2246822507u);
    uint _388 = spgiJitterHash(param_4);
    uint sampleHash = _388;
    float jitter = (float(sampleHash & 16777215u) + 0.5f) / 16777216.0f;
    float u = ((float(rayIdx) + jitter) / float(n)) * sum;
    uint c = n - 1u;
    float acc = 0.0f;
    for (uint i_2 = 0u; i_2 < n; i_2++)
    {
        acc += pdf[i_2];
        if (u <= acc)
        {
            c = i_2;
            break;
        }
    }
    invNq = sum / (float(n) * max(pdf[c], 9.9999999747524270787835121154785e-07f));
    uint param_5 = c;
    uint param_6 = n;
    return mul(fibonacciDirection(param_5, param_6), rot);
}

float3 orientAroundNormalTBN(float3 dir, float3 normal)
{
    bool3 _98 = (abs(normal.y) < 0.999000012874603271484375f).xxx;
    float3 up = float3(_98.x ? float3(0.0f, 1.0f, 0.0f).x : float3(1.0f, 0.0f, 0.0f).x, _98.y ? float3(0.0f, 1.0f, 0.0f).y : float3(1.0f, 0.0f, 0.0f).y, _98.z ? float3(0.0f, 1.0f, 0.0f).z : float3(1.0f, 0.0f, 0.0f).z);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(((tangent * dir.x) + (bitangent * dir.y)) + (normal * dir.z));
}

float NDCDepthToLinear(float ndcDepth, float nearPlane, float farPlane)
{
    return (nearPlane * farPlane) / (farPlane - (ndcDepth * (farPlane - nearPlane)));
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    uint gridW = uint(global_grid_params.x);
    uint gridH = uint(global_grid_params.y);
    uint raysPerProbe = uint(global_grid_params.w);
    uint probeIdx;
    uint rayIdx;
    if (global_tier_params.x > 0.5f)
    {
        probeIdx = compactList.Load((global_id / raysPerProbe) * 4 + 0);
        if (probeIdx == 4294967295u)
        {
            return;
        }
        rayIdx = global_id % raysPerProbe;
        bool _527 = global_tier_params.w > 0.5f;
        bool _540;
        if (_527)
        {
            _540 = float(rayIdx) >= asfloat(probeNormals.Load(probeIdx * 16 + 12));
        }
        else
        {
            _540 = _527;
        }
        if (_540)
        {
            return;
        }
    }
    else
    {
        uint totalRays = (gridW * gridH) * raysPerProbe;
        if (global_id >= totalRays)
        {
            return;
        }
        probeIdx = global_id / raysPerProbe;
        rayIdx = global_id % raysPerProbe;
    }
    hit_buffer.Store4(global_id * 16 + 0, asuint(float4(-1.0f, 0.0f, 0.0f, 0.0f)));
    float4 posData = asfloat(probePositions.Load4(probeIdx * 16 + 0));
    if (posData.w <= 0.0f)
    {
        return;
    }
    float3 probeNormal = normalize(asfloat(probeNormals.Load4(probeIdx * 16 + 0)).xyz);
    float3 probePos = posData.xyz;
    float3 rayDir;
    if (global_cache_params.y > 0.5f)
    {
        uint hb = probeIdx * 4u;
        float4 h0 = asfloat(importanceSH.Load4((hb + 0u) * 16 + 0));
        float4 h1 = asfloat(importanceSH.Load4((hb + 1u) * 16 + 0));
        float4 h2 = asfloat(importanceSH.Load4((hb + 2u) * 16 + 0));
        float4 h3 = asfloat(importanceSH.Load4((hb + 3u) * 16 + 0));
        uint param = rayIdx;
        uint param_1 = raysPerProbe;
        uint param_2 = probeIdx;
        float param_3 = global_camera_position.w;
        float3 param_4 = probeNormal;
        float3 param_5 = h0.xyz;
        float3 param_6 = h1.xyz;
        float3 param_7 = h2.xyz;
        float3 param_8 = h3.xyz;
        float param_9;
        float3 _656 = spgiImportanceDirection(param, param_1, param_2, param_3, param_4, param_5, param_6, param_7, param_8, param_9);
        float invNq = param_9;
        rayDir = _656;
        float3 param_10 = rayDir;
        float3 param_11 = probeNormal;
        rayDir = orientAroundNormalTBN(param_10, param_11);
    }
    else
    {
        uint param_12 = rayIdx;
        uint param_13 = raysPerProbe;
        rayDir = fibonacciDirection(param_12, param_13);
        uint param_14 = probeIdx;
        float param_15 = global_camera_position.w;
        rayDir = mul(rayDir, spgiJitterRotation(param_14, param_15));
        float3 param_16 = rayDir;
        float3 param_17 = probeNormal;
        rayDir = orientAroundNormalTBN(param_16, param_17);
    }
    float3 origin = probePos + ((probeNormal * global_sdf_voxel_sizes[0].x) * 2.0f);
    float near = global_hzb_params.x;
    float far = global_hzb_params.y;
    uint mipLevels = max(uint(global_hzb_params.z), 1u);
    float maxDist = global_hzb_params.w;
    if (maxDist <= 0.0f)
    {
        return;
    }
    float baseStep = maxDist / 32.0f;
    float t = 0.0f;
    uint mip = 1u;
    float thickness = 0.300000011920928955078125f;
    float4 clip0 = mul(float4(origin, 1.0f), global_view_projection);
    float4 clip1 = mul(float4(origin + (rayDir * maxDist), 1.0f), global_view_projection);
    bool _744 = clip0.w <= 0.0f;
    bool _751;
    if (!_744)
    {
        _751 = clip1.w <= 0.0f;
    }
    else
    {
        _751 = _744;
    }
    if (_751)
    {
        return;
    }
    float2 uv0 = ((clip0.xy / clip0.w.xx) * 0.5f) + 0.5f.xx;
    float2 uv1 = ((clip1.xy / clip1.w.xx) * 0.5f) + 0.5f.xx;
    if (distance(uv0, uv1) > 1.5f)
    {
        return;
    }
    for (uint _step = 0u; _step < 32u; _step++)
    {
        float mipScale = float(1u << mip);
        t += (baseStep * mipScale);
        if (t > maxDist)
        {
            break;
        }
        float3 world = origin + (rayDir * t);
        float4 clip = mul(float4(world, 1.0f), global_view_projection);
        if (clip.w <= 0.0f)
        {
            break;
        }
        float3 ndc = clip.xyz / clip.w.xxx;
        float2 uv = float2((ndc.x * 0.5f) + 0.5f, 1.0f - ((ndc.y * 0.5f) + 0.5f));
        bool _850 = uv.x < 0.0f;
        bool _857;
        if (!_850)
        {
            _857 = uv.x > 1.0f;
        }
        else
        {
            _857 = _850;
        }
        bool _864;
        if (!_857)
        {
            _864 = uv.y < 0.0f;
        }
        else
        {
            _864 = _857;
        }
        bool _871;
        if (!_864)
        {
            _871 = uv.y > 1.0f;
        }
        else
        {
            _871 = _864;
        }
        if (_871)
        {
            break;
        }
        uint _884_dummy_parameter;
        int2 mipSize = int2(spvTextureSize(hzb_texture, uint(int(mip)), _884_dummy_parameter));
        bool _888 = mipSize.x <= 0;
        bool _895;
        if (!_888)
        {
            _895 = mipSize.y <= 0;
        }
        else
        {
            _895 = _888;
        }
        if (_895)
        {
            break;
        }
        int2 mipCoord = clamp(int2(uv * float2(mipSize)), int2(0, 0), mipSize - int2(1, 1));
        float sceneNDC = hzb_texture.Load(int3(mipCoord, int(mip))).x;
        float param_18 = sceneNDC;
        float param_19 = near;
        float param_20 = far;
        float sceneLin = NDCDepthToLinear(param_18, param_19, param_20);
        float param_21 = ndc.z;
        float param_22 = near;
        float param_23 = far;
        float rayLin = NDCDepthToLinear(param_21, param_22, param_23);
        if (rayLin > sceneLin)
        {
            if (mip == 0u)
            {
                if (rayLin <= (sceneLin + thickness))
                {
                    uint _952_dummy_parameter;
                    float2 dims = float2(int2(spvTextureSize(hzb_texture, uint(0), _952_dummy_parameter)));
                    float2 center = (floor(uv * dims) + 0.5f.xx) / dims;
                    float2 screenNDC = float2((center.x * 2.0f) - 1.0f, 1.0f - (center.y * 2.0f));
                    float4 hp = mul(float4(screenNDC, sceneNDC, 1.0f), global_inv_view_projection);
                    if (abs(hp.w) < 9.9999999747524270787835121154785e-07f)
                    {
                        continue;
                    }
                    float3 surface = hp.xyz / hp.w.xxx;
                    float4 px = mul(float4(screenNDC + float2(2.0f / dims.x, 0.0f), sceneNDC, 1.0f), global_inv_view_projection);
                    if (abs(px.w) < 9.9999999747524270787835121154785e-07f)
                    {
                        continue;
                    }
                    float footprint = distance(surface, px.xyz / px.w.xxx);
                    float alongRay = dot(surface - origin, rayDir);
                    float rayError = distance(surface, origin + (rayDir * alongRay));
                    bool _1045 = (alongRay <= 0.0f) || (alongRay > maxDist);
                    bool _1054;
                    if (!_1045)
                    {
                        _1054 = rayError > max(0.0199999995529651641845703125f, footprint * 1.5f);
                    }
                    else
                    {
                        _1054 = _1045;
                    }
                    if (_1054)
                    {
                        continue;
                    }
                    hit_buffer.Store4(global_id * 16 + 0, asuint(float4(alongRay, center.x, center.y, 1.0f)));
                    return;
                }
            }
            else
            {
                t -= (baseStep * mipScale);
                mip--;
            }
        }
        else
        {
            mip = min((mip + 1u), (mipLevels - 1u));
        }
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
