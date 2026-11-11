static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer GlobalBlock : register(b4, space0)
{
    row_major float4x4 global_view_projection : packoffset(c0);
    row_major float4x4 global_inv_view_projection : packoffset(c4);
    float4 global_camera_position : packoffset(c8);
    float4 global_grid_params : packoffset(c9);
    float4 global_trace_params : packoffset(c10);
    float4 global_sdf_origins[3] : packoffset(c11);
    float4 global_sdf_voxel_sizes[3] : packoffset(c14);
    float4 global_sdf_extents[3] : packoffset(c17);
    uint4 global_sdf_resolutions : packoffset(c20);
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
};

ByteAddressBuffer tileLightVar : register(t5, space0);
RWByteAddressBuffer probePositions : register(u2, space0);
RWByteAddressBuffer probeNormals : register(u3, space0);
Texture2D<float4> normalTexture : register(t1, space0);
Texture2D<float4> depthTexture : register(t0, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint spgiJitterHash(inout uint v)
{
    v ^= (v >> 16u);
    v *= 2146121005u;
    v ^= (v >> 15u);
    v *= 2221713035u;
    v ^= (v >> 16u);
    return v;
}

float2 spgiJitterOffset01(uint probeIdx, float frameSeed)
{
    if (frameSeed < 0.0f)
    {
        return 0.0f.xx;
    }
    uint param = uint(frameSeed) ^ (probeIdx * 2654435769u);
    uint _100 = spgiJitterHash(param);
    uint h = _100;
    return (float2(float(h & 65535u), float((h >> 16u) & 65535u)) * 3.05180437862873077392578125e-05f) - 1.0f.xx;
}

int2 clampTexel(float2 pixel, float renderW, float renderH)
{
    return int2(clamp(pixel, 0.0f.xx, float2(renderW - 1.0f, renderH - 1.0f)));
}

bool depthValid(float d)
{
    return (d > 9.9999997473787516355514526367188e-05f) && (d < 0.999000012874603271484375f);
}

float3 reconstructWorldPos(float2 uv, float depth, float4x4 invVP)
{
    float2 ndc = float2((uv.x * 2.0f) - 1.0f, 1.0f - (uv.y * 2.0f));
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, invVP);
    return worldPos.xyz / worldPos.w.xxx;
}

int pickRepresentative(float d0, float d1, float d2, float d3, float d4, int2 t0, int2 t1, int2 t2, int2 t3, int2 t4, float wDepth, float wNormal, float renderW, float renderH)
{
    float _178[5] = { d0, d1, d2, d3, d4 };
    float d[5] = _178;
    int2 _187[5] = { t0, t1, t2, t3, t4 };
    int2 t[5] = _187;
    int n = 0;
    float zd[5];
    int idx[5];
    for (int i = 0; i < 5; i++)
    {
        float param = d[i];
        if (!depthValid(param))
        {
            continue;
        }
        int j = n - 1;
        for (;;)
        {
            bool _219 = j >= 0;
            bool _230;
            if (_219)
            {
                _230 = zd[j] > d[i];
            }
            else
            {
                _230 = _219;
            }
            if (_230)
            {
                zd[j + 1] = zd[j];
                idx[j + 1] = idx[j];
                j--;
                continue;
            }
            else
            {
                break;
            }
        }
        zd[j + 1] = d[i];
        idx[j + 1] = i;
        n++;
    }
    if (n == 0)
    {
        return -1;
    }
    float zMed = zd[n / 2];
    float3 nMed = normalize((normalTexture.Load(int3(t[idx[n / 2]], 0)).xyz * 2.0f) - 1.0f.xxx);
    int best = -1;
    float bestCost = 3.3999999521443642490773241379936e+38f;
    for (int i_1 = 0; i_1 < 5; i_1++)
    {
        float param_1 = d[i_1];
        if (!depthValid(param_1))
        {
            continue;
        }
        float3 np = normalize((normalTexture.Load(int3(t[i_1], 0)).xyz * 2.0f) - 1.0f.xxx);
        float cost = (wDepth * abs(d[i_1] - zMed)) + (wNormal * (1.0f - max(dot(np, nMed), 0.0f)));
        if (cost < bestCost)
        {
            bestCost = cost;
            best = i_1;
        }
    }
    return best;
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    uint gridW = uint(global_grid_params.x);
    uint gridH = uint(global_grid_params.y);
    uint totalTiles = gridW * gridH;
    bool tier3 = global_tier_params.x > 0.5f;
    uint _392;
    if (tier3)
    {
        _392 = totalTiles * 4u;
    }
    else
    {
        _392 = totalTiles;
    }
    uint totalSlots = _392;
    if (global_id >= totalSlots)
    {
        return;
    }
    uint slot = 0u;
    uint tileIdx = global_id;
    if (tier3)
    {
        slot = global_id & 3u;
        tileIdx = global_id >> 2u;
    }
    uint gx = tileIdx % gridW;
    uint gy = tileIdx / gridW;
    float downsample = global_grid_params.z;
    float renderW = global_trace_params.z;
    float renderH = global_trace_params.w;
    uint probeIdx = global_id;
    float frameSeed = global_camera_position.w;
    float jitterMaxWorld = global_surface_cache_params.w;
    uint gridRays = uint(global_grid_params.w);
    if (tier3 && (slot != 0u))
    {
        float2 quadOff = 0.25f.xx;
        if (slot == 2u)
        {
            quadOff = float2(0.75f, 0.25f);
        }
        else
        {
            if (slot == 3u)
            {
                quadOff = 0.75f.xx;
            }
        }
        float2 pixelCenter = float2(float(gx) * downsample, float(gy) * downsample) + (quadOff * downsample);
        float2 _489;
        if ((frameSeed >= 0.0f) && (jitterMaxWorld > 0.0f))
        {
            uint param = probeIdx;
            float param_1 = frameSeed;
            _489 = spgiJitterOffset01(param, param_1) * (downsample * 0.25f);
        }
        else
        {
            _489 = 0.0f.xx;
        }
        float2 jitterPx = _489;
        float2 pixel = pixelCenter + jitterPx;
        float2 param_2 = pixel;
        float param_3 = renderW;
        float param_4 = renderH;
        int2 texel = clampTexel(param_2, param_3, param_4);
        float depth = depthTexture.Load(int3(texel, 0)).x;
        float param_5 = depth;
        bool isActive = depthValid(param_5);
        if (isActive)
        {
            float2 param_6 = float2((float(gx) * downsample) + (downsample * 0.5f), (float(gy) * downsample) + (downsample * 0.5f));
            float param_7 = renderW;
            float param_8 = renderH;
            int2 anchorTexel = clampTexel(param_6, param_7, param_8);
            float anchorDepth = depthTexture.Load(int3(anchorTexel, 0)).x;
            float param_9 = anchorDepth;
            if (depthValid(param_9))
            {
                float2 anchorUV = (float2(anchorTexel) + 0.5f.xx) / float2(renderW, renderH);
                float2 param_10 = anchorUV;
                float param_11 = anchorDepth;
                float4x4 param_12 = global_inv_view_projection;
                float3 anchorPos = reconstructWorldPos(param_10, param_11, param_12);
                float2 uv = (float2(texel) + 0.5f.xx) / float2(renderW, renderH);
                float2 param_13 = uv;
                float param_14 = depth;
                float4x4 param_15 = global_inv_view_projection;
                float3 worldPos = reconstructWorldPos(param_13, param_14, param_15);
                isActive = distance(worldPos, anchorPos) > global_cache_params.z;
                bool _604 = !isActive;
                bool _610;
                if (_604)
                {
                    _610 = global_tier_params.y > 0.0f;
                }
                else
                {
                    _610 = _604;
                }
                if (_610)
                {
                    float3 nQ = normalize((normalTexture.Load(int3(texel, 0)).xyz * 2.0f) - 1.0f.xxx);
                    float3 nA = normalize((normalTexture.Load(int3(anchorTexel, 0)).xyz * 2.0f) - 1.0f.xxx);
                    isActive = dot(nQ, nA) < global_tier_params.y;
                }
            }
            else
            {
                isActive = true;
            }
            bool _640 = !isActive;
            bool _646;
            if (_640)
            {
                _646 = global_tier_params.z > 0.5f;
            }
            else
            {
                _646 = _640;
            }
            if (_646)
            {
                isActive = tileLightVar.Load(tileIdx * 4 + 0) != 0u;
            }
        }
        if (!isActive)
        {
            probePositions.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
            probeNormals.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
            return;
        }
        float2 uv_1 = (float2(texel) + 0.5f.xx) / float2(renderW, renderH);
        float2 param_16 = uv_1;
        float param_17 = depth;
        float4x4 param_18 = global_inv_view_projection;
        float3 worldPos_1 = reconstructWorldPos(param_16, param_17, param_18);
        float3 normal = normalize((normalTexture.Load(int3(texel, 0)).xyz * 2.0f) - 1.0f.xxx);
        float _708;
        if (global_tier_params.w > 0.5f)
        {
            _708 = 32.0f;
        }
        else
        {
            _708 = float(gridRays);
        }
        float tierRays = _708;
        probePositions.Store4(probeIdx * 16 + 0, asuint(float4(worldPos_1, 1.0f)));
        probeNormals.Store4(probeIdx * 16 + 0, asuint(float4(normal, tierRays)));
        return;
    }
    float2 pixelCenter_1 = float2((float(gx) * downsample) + (downsample * 0.5f), (float(gy) * downsample) + (downsample * 0.5f));
    float2 param_19 = pixelCenter_1;
    float param_20 = renderW;
    float param_21 = renderH;
    int2 repTexel = clampTexel(param_19, param_20, param_21);
    float repDepth = depthTexture.Load(int3(repTexel, 0)).x;
    if (global_repr_params.x > 0.5f)
    {
        float halfCell = downsample * 0.5f;
        float2 _783[5] = { 0.0f.xx, float2(-halfCell, 0.0f), float2(halfCell, 0.0f), float2(0.0f, -halfCell), float2(0.0f, halfCell) };
        float2 offsets[5] = _783;
        int2 ct[5];
        float cd[5];
        for (int i = 0; i < 5; i++)
        {
            float2 param_22 = pixelCenter_1 + offsets[i];
            float param_23 = renderW;
            float param_24 = renderH;
            ct[i] = clampTexel(param_22, param_23, param_24);
            cd[i] = depthTexture.Load(int3(ct[i], 0)).x;
        }
        float param_25 = cd[0];
        float param_26 = cd[1];
        float param_27 = cd[2];
        float param_28 = cd[3];
        float param_29 = cd[4];
        int2 param_30 = ct[0];
        int2 param_31 = ct[1];
        int2 param_32 = ct[2];
        int2 param_33 = ct[3];
        int2 param_34 = ct[4];
        float param_35 = global_repr_params.y;
        float param_36 = global_repr_params.z;
        float param_37 = renderW;
        float param_38 = renderH;
        int pick = pickRepresentative(param_25, param_26, param_27, param_28, param_29, param_30, param_31, param_32, param_33, param_34, param_35, param_36, param_37, param_38);
        if (pick < 0)
        {
            probePositions.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
            probeNormals.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
            return;
        }
        repTexel = ct[pick];
        repDepth = cd[pick];
        pixelCenter_1 = float2(repTexel) + 0.5f.xx;
    }
    float2 _884;
    if ((frameSeed >= 0.0f) && (jitterMaxWorld > 0.0f))
    {
        uint param_39 = probeIdx;
        float param_40 = frameSeed;
        _884 = spgiJitterOffset01(param_39, param_40) * (downsample * 0.25f);
    }
    else
    {
        _884 = 0.0f.xx;
    }
    float2 jitterPx_1 = _884;
    float2 pixel_1 = pixelCenter_1 + jitterPx_1;
    float2 param_41 = pixel_1;
    float param_42 = renderW;
    float param_43 = renderH;
    int2 depthTexel = clampTexel(param_41, param_42, param_43);
    float2 uv_2 = (float2(depthTexel) + 0.5f.xx) / float2(renderW, renderH);
    float depth_1 = depthTexture.Load(int3(depthTexel, 0)).x;
    if (any(bool2(jitterPx_1.x != 0.0f.xx.x, jitterPx_1.y != 0.0f.xx.y)))
    {
        float depthC = depthTexture.Load(int3(repTexel, 0)).x;
        float2 repUV = (float2(repTexel) + 0.5f.xx) / float2(renderW, renderH);
        float2 param_44 = repUV;
        float param_45 = depthC;
        float4x4 param_46 = global_inv_view_projection;
        float3 worldC = reconstructWorldPos(param_44, param_45, param_46);
        float2 param_47 = uv_2;
        float param_48 = depth_1;
        float4x4 param_49 = global_inv_view_projection;
        float3 worldJ = reconstructWorldPos(param_47, param_48, param_49);
        float delta = distance(worldC, worldJ);
        if (delta > jitterMaxWorld)
        {
            pixel_1 = pixelCenter_1 + (jitterPx_1 * (jitterMaxWorld / max(delta, 9.9999997473787516355514526367188e-06f)));
            float2 param_50 = pixel_1;
            float param_51 = renderW;
            float param_52 = renderH;
            depthTexel = clampTexel(param_50, param_51, param_52);
            uv_2 = (float2(depthTexel) + 0.5f.xx) / float2(renderW, renderH);
            depth_1 = depthTexture.Load(int3(depthTexel, 0)).x;
        }
    }
    float param_53 = depth_1;
    bool isActive_1 = depthValid(param_53);
    bool _1005 = isActive_1 && (!tier3);
    bool _1013;
    if (_1005)
    {
        _1013 = ((gx + gy) & 1u) != 0u;
    }
    else
    {
        _1013 = _1005;
    }
    if (_1013)
    {
        float posErrThresh = global_cache_params.z;
        bool highVariance;
        if (posErrThresh > 0.0f)
        {
            float2 param_54 = uv_2;
            float param_55 = depth_1;
            float4x4 param_56 = global_inv_view_projection;
            float3 posC = reconstructWorldPos(param_54, param_55, param_56);
            int2 maxTexel = int2(int(renderW) - 1, int(renderH) - 1);
            int2 _1067[4] = { int2(int(gx - 1u), int(gy)), int2(int(gx + 1u), int(gy)), int2(int(gx), int(gy - 1u)), int2(int(gx), int(gy + 1u)) };
            int2 ncell[4] = _1067;
            bool interpFailed = false;
            float3 acc = 0.0f.xxx;
            float wsum = 0.0f;
            for (int i_1 = 0; i_1 < 4; i_1++)
            {
                int2 cc = ncell[i_1];
                bool _1087 = cc.x < 0;
                bool _1094;
                if (!_1087)
                {
                    _1094 = cc.y < 0;
                }
                else
                {
                    _1094 = _1087;
                }
                bool _1103;
                if (!_1094)
                {
                    _1103 = cc.x >= int(gridW);
                }
                else
                {
                    _1103 = _1094;
                }
                bool _1112;
                if (!_1103)
                {
                    _1112 = cc.y >= int(gridH);
                }
                else
                {
                    _1112 = _1103;
                }
                if (_1112)
                {
                    continue;
                }
                int2 tex = clamp(int2((cc.x * int(downsample)) + (int(downsample) / 2), (cc.y * int(downsample)) + (int(downsample) / 2)), int2(0, 0), maxTexel);
                float d = depthTexture.Load(int3(tex, 0)).x;
                if ((d <= 9.9999997473787516355514526367188e-05f) || (d >= 0.999000012874603271484375f))
                {
                    interpFailed = true;
                    break;
                }
                float2 uvN = (float2(tex) + 0.5f.xx) / float2(renderW, renderH);
                float2 param_57 = uvN;
                float param_58 = d;
                float4x4 param_59 = global_inv_view_projection;
                acc += (reconstructWorldPos(param_57, param_58, param_59) * 0.25f);
                wsum += 0.25f;
            }
            if ((!interpFailed) && (wsum > 0.0f))
            {
                float scale = 1.0f + (1.0f - wsum);
                interpFailed = distance(acc / wsum.xxx, posC) > (posErrThresh * scale);
            }
            bool _1201 = (!interpFailed) && (wsum > 0.0f);
            bool _1207;
            if (_1201)
            {
                _1207 = global_repr_params.w > 0.5f;
            }
            else
            {
                _1207 = _1201;
            }
            if (_1207)
            {
                float3 nC = normalize((normalTexture.Load(int3(depthTexel, 0)).xyz * 2.0f) - 1.0f.xxx);
                float minNDot = 1.0f;
                for (int i_2 = 0; i_2 < 4; i_2++)
                {
                    int2 cc_1 = ncell[i_2];
                    bool _1234 = cc_1.x < 0;
                    bool _1241;
                    if (!_1234)
                    {
                        _1241 = cc_1.y < 0;
                    }
                    else
                    {
                        _1241 = _1234;
                    }
                    bool _1250;
                    if (!_1241)
                    {
                        _1250 = cc_1.x >= int(gridW);
                    }
                    else
                    {
                        _1250 = _1241;
                    }
                    bool _1259;
                    if (!_1250)
                    {
                        _1259 = cc_1.y >= int(gridH);
                    }
                    else
                    {
                        _1259 = _1250;
                    }
                    if (_1259)
                    {
                        continue;
                    }
                    int2 tex_1 = clamp(int2((cc_1.x * int(downsample)) + (int(downsample) / 2), (cc_1.y * int(downsample)) + (int(downsample) / 2)), int2(0, 0), maxTexel);
                    float3 n = normalize((normalTexture.Load(int3(tex_1, 0)).xyz * 2.0f) - 1.0f.xxx);
                    minNDot = min(minNDot, dot(nC, n));
                }
                interpFailed = minNDot < global_tier_params.y;
            }
            highVariance = interpFailed || (wsum <= 0.0f);
        }
        else
        {
            float3 nC_1 = normalTexture.Load(int3(depthTexel, 0)).xyz;
            nC_1 = normalize((nC_1 * 2.0f) - 1.0f.xxx);
            float dMin = depth_1;
            float dMax = depth_1;
            float minNDot_1 = 1.0f;
            int2 halfBlock = (int(downsample) / 2).xx;
            int2 _1352[4] = { depthTexel + int2(halfBlock.x, 0), depthTexel - int2(halfBlock.x, 0), depthTexel + int2(0, halfBlock.y), depthTexel - int2(0, halfBlock.y) };
            int2 cardinals[4] = _1352;
            int2 maxTexel_1 = int2(int(renderW) - 1, int(renderH) - 1);
            for (int i_3 = 0; i_3 < 4; i_3++)
            {
                int2 t = clamp(cardinals[i_3], int2(0, 0), maxTexel_1);
                float d_1 = depthTexture.Load(int3(t, 0)).x;
                dMin = min(dMin, d_1);
                dMax = max(dMax, d_1);
                float3 n_1 = normalTexture.Load(int3(t, 0)).xyz;
                n_1 = normalize((n_1 * 2.0f) - 1.0f.xxx);
                minNDot_1 = min(minNDot_1, dot(nC_1, n_1));
            }
            highVariance = ((dMax - dMin) > 0.00150000001303851604461669921875f) || (minNDot_1 < 0.800000011920928955078125f);
        }
        if (!highVariance)
        {
            isActive_1 = false;
        }
    }
    if (!isActive_1)
    {
        probePositions.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
        probeNormals.Store4(probeIdx * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    float2 param_60 = uv_2;
    float param_61 = depth_1;
    float4x4 param_62 = global_inv_view_projection;
    float3 worldPos_2 = reconstructWorldPos(param_60, param_61, param_62);
    float3 normal_1 = normalTexture.Load(int3(depthTexel, 0)).xyz;
    normal_1 = normalize((normal_1 * 2.0f) - 1.0f.xxx);
    float _1446;
    if (tier3)
    {
        float _1452;
        if (global_tier_params.w > 0.5f)
        {
            _1452 = 16.0f;
        }
        else
        {
            _1452 = float(gridRays);
        }
        _1446 = _1452;
    }
    else
    {
        _1446 = 1.0f;
    }
    float tierRays_1 = _1446;
    probePositions.Store4(probeIdx * 16 + 0, asuint(float4(worldPos_2, 1.0f)));
    probeNormals.Store4(probeIdx * 16 + 0, asuint(float4(normal_1, tierRays_1)));
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
