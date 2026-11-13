static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer TemporalConstantsBlock : register(b5, space0)
{
    row_major float4x4 params_prev_view_projection : packoffset(c0);
    uint params_totalProbes : packoffset(c4);
    float params_baseAlpha : packoffset(c4.y);
    float params_posThreshold : packoffset(c4.z);
    float params_normalThreshold : packoffset(c4.w);
    float params_clampScale : packoffset(c5);
    float params_maxGradient : packoffset(c5.y);
    uint params_gridW : packoffset(c5.z);
    uint params_gridH : packoffset(c5.w);
    float params_mode : packoffset(c6);
    float params_pad0 : packoffset(c6.y);
};

ByteAddressBuffer currentPositions : register(t2, space0);
RWByteAddressBuffer outputSH : register(u4, space0);
ByteAddressBuffer currentSH : register(t0, space0);
ByteAddressBuffer currentNormals : register(t6, space0);
ByteAddressBuffer historyPositions : register(t3, space0);
ByteAddressBuffer historyNormals : register(t7, space0);
ByteAddressBuffer historySH : register(t1, space0);

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

float4 reprojectPrevUV(float3 worldPos, float4x4 prevVP)
{
    float4 clip = mul(float4(worldPos, 1.0f), prevVP);
    if (clip.w <= 0.0f)
    {
        return float4(0.0f, 0.0f, clip.w, 0.0f);
    }
    float2 ndc = clip.xy / clip.w.xx;
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, 1.0f - ((ndc.y * 0.5f) + 0.5f));
    return float4(uv, clip.w, 1.0f);
}

void varianceClampAABB(float3 history, float3 mean, float3 sigma, float scale, out float3 clamped)
{
    float3 sigmaFloor = max(abs(mean) * 0.100000001490116119384765625f, 0.00999999977648258209228515625f.xxx);
    float3 aabbMin = (mean - (sigma * scale)) - sigmaFloor;
    float3 aabbMax = (mean + (sigma * scale)) + sigmaFloor;
    clamped = clamp(history, aabbMin, aabbMax);
}

void comp_main()
{
    uint global_id = gl_GlobalInvocationID.x;
    if (global_id >= params_totalProbes)
    {
        return;
    }
    float4 curPos = asfloat(currentPositions.Load4(global_id * 16 + 0));
    if (curPos.w <= 0.0f)
    {
        uint base = global_id * 4u;
        outputSH.Store4((base + 0u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 1u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 2u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base + 3u) * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    uint base_1 = global_id * 4u;
    float4 curSH0 = asfloat(currentSH.Load4((base_1 + 0u) * 16 + 0));
    float probeConfidence = curSH0.w;
    if (probeConfidence <= 0.0f)
    {
        outputSH.Store4((base_1 + 0u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base_1 + 1u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base_1 + 2u) * 16 + 0, asuint(0.0f.xxxx));
        outputSH.Store4((base_1 + 3u) * 16 + 0, asuint(0.0f.xxxx));
        return;
    }
    float histW = 0.0f;
    bool historyValid = false;
    float3 param = curPos.xyz;
    float4x4 param_1 = params_prev_view_projection;
    float4 rp = reprojectPrevUV(param, param_1);
    float4 histSH[4];
    if (rp.w > 0.0f)
    {
        float2 prevUV = rp.xy;
        bool _224 = all(bool2(prevUV.x >= 0.0f.xx.x, prevUV.y >= 0.0f.xx.y));
        bool _231;
        if (_224)
        {
            _231 = all(bool2(prevUV.x <= 1.0f.xx.x, prevUV.y <= 1.0f.xx.y));
        }
        else
        {
            _231 = _224;
        }
        if (_231)
        {
            float2 cellF = (prevUV * float2(float(params_gridW), float(params_gridH))) - 0.5f.xx;
            int2 c0 = int2(floor(cellF));
            float2 fr = frac(cellF);
            float4 curNorm = asfloat(currentNormals.Load4(global_id * 16 + 0));
            for (int b = 0; b < 4; b++)
            {
                histSH[b] = 0.0f.xxxx;
            }
            float _444;
            float _456;
            for (int dy = 0; dy <= 1; dy++)
            {
                for (int dx = 0; dx <= 1; dx++)
                {
                    int2 c = c0 + int2(dx, dy);
                    bool _306 = c.x < 0;
                    bool _313;
                    if (!_306)
                    {
                        _313 = c.y < 0;
                    }
                    else
                    {
                        _313 = _306;
                    }
                    bool _323;
                    if (!_313)
                    {
                        _323 = c.x >= int(params_gridW);
                    }
                    else
                    {
                        _323 = _313;
                    }
                    bool _333;
                    if (!_323)
                    {
                        _333 = c.y >= int(params_gridH);
                    }
                    else
                    {
                        _333 = _323;
                    }
                    if (_333)
                    {
                        continue;
                    }
                    uint hIdx = (uint(c.y) * params_gridW) + uint(c.x);
                    float4 hPos = asfloat(historyPositions.Load4(hIdx * 16 + 0));
                    if (hPos.w <= 0.0f)
                    {
                        continue;
                    }
                    if (distance(curPos.xyz, hPos.xyz) > params_posThreshold)
                    {
                        continue;
                    }
                    float4 hNorm = asfloat(historyNormals.Load4(hIdx * 16 + 0));
                    bool _385 = hNorm.w > 0.0f;
                    bool _391;
                    if (_385)
                    {
                        _391 = curNorm.w > 0.0f;
                    }
                    else
                    {
                        _391 = _385;
                    }
                    bool _401;
                    if (_391)
                    {
                        _401 = dot(curNorm.xyz, curNorm.xyz) > 9.9999999392252902907785028219223e-09f;
                    }
                    else
                    {
                        _401 = _391;
                    }
                    bool _410;
                    if (_401)
                    {
                        _410 = dot(hNorm.xyz, hNorm.xyz) > 9.9999999392252902907785028219223e-09f;
                    }
                    else
                    {
                        _410 = _401;
                    }
                    bool _423;
                    if (_410)
                    {
                        _423 = dot(normalize(curNorm.xyz), normalize(hNorm.xyz)) < params_normalThreshold;
                    }
                    else
                    {
                        _423 = _410;
                    }
                    if (_423)
                    {
                        continue;
                    }
                    uint hb = hIdx * 4u;
                    if (asfloat(historySH.Load(hb * 16 + 12)) <= 0.0f)
                    {
                        continue;
                    }
                    if (dx == 0)
                    {
                        _444 = 1.0f - fr.x;
                    }
                    else
                    {
                        _444 = fr.x;
                    }
                    if (dy == 0)
                    {
                        _456 = 1.0f - fr.y;
                    }
                    else
                    {
                        _456 = fr.y;
                    }
                    float w = _444 * _456;
                    for (int b_1 = 0; b_1 < 4; b_1++)
                    {
                        histSH[b_1] += (asfloat(historySH.Load4((hb + uint(b_1)) * 16 + 0)) * w);
                    }
                    histW += w;
                }
            }
            if (histW > 0.001000000047497451305389404296875f)
            {
                historyValid = true;
                for (int b_2 = 0; b_2 < 4; b_2++)
                {
                    histSH[b_2] /= histW.xxxx;
                }
            }
        }
    }
    if (!historyValid)
    {
        float4 _545;
        for (int b_3 = 0; b_3 < 4; b_3++)
        {
            float4 cur = asfloat(currentSH.Load4((base_1 + uint(b_3)) * 16 + 0));
            if (b_3 == 0)
            {
                _545 = float4(cur.xyz, probeConfidence);
            }
            else
            {
                _545 = float4(cur.xyz, cur.w);
            }
            outputSH.Store4((base_1 + uint(b_3)) * 16 + 0, asuint(_545));
        }
        return;
    }
    if (probeConfidence < 0.0500000007450580596923828125f)
    {
        float4 _595;
        for (int b_4 = 0; b_4 < 4; b_4++)
        {
            float4 cur_1 = asfloat(currentSH.Load4((base_1 + uint(b_4)) * 16 + 0));
            if (b_4 == 0)
            {
                _595 = float4(cur_1.xyz, probeConfidence);
            }
            else
            {
                _595 = float4(cur_1.xyz, cur_1.w);
            }
            outputSH.Store4((base_1 + uint(b_4)) * 16 + 0, asuint(_595));
        }
        return;
    }
    bool _622 = params_mode > 0.5f;
    bool _629;
    if (_622)
    {
        _629 = params_mode < 1.5f;
    }
    else
    {
        _629 = _622;
    }
    if (_629)
    {
        float3 sum = curSH0.xyz;
        float3 sumSq = curSH0.xyz * curSH0.xyz;
        float n = 1.0f;
        uint gx = global_id % params_gridW;
        if (gx > 0u)
        {
            float4 v = asfloat(currentSH.Load4(((global_id - 1u) * 4u) * 16 + 0));
            if (v.w > 0.0f)
            {
                sum += v.xyz;
                sumSq += (v.xyz * v.xyz);
                n += 1.0f;
            }
        }
        if ((gx + 1u) < params_gridW)
        {
            float4 v_1 = asfloat(currentSH.Load4(((global_id + 1u) * 4u) * 16 + 0));
            if (v_1.w > 0.0f)
            {
                sum += v_1.xyz;
                sumSq += (v_1.xyz * v_1.xyz);
                n += 1.0f;
            }
        }
        float3 mean = sum / n.xxx;
        float3 sigma = sqrt(max((sumSq / n.xxx) - (mean * mean), 0.0f.xxx));
        float3 param_2 = histSH[0].xyz;
        float3 param_3 = mean;
        float3 param_4 = sigma;
        float param_5 = params_clampScale;
        float3 param_6;
        varianceClampAABB(param_2, param_3, param_4, param_5, param_6);
        float3 cl0 = param_6;
        float3 shrink = clamp(abs(cl0) / max(abs(histSH[0].xyz), 9.9999997473787516355514526367188e-05f.xxx), 0.0f.xxx, 1.0f.xxx);
        histSH[0].x = cl0.x;
        histSH[0].y = cl0.y;
        histSH[0].z = cl0.z;
        float4 _761 = histSH[1];
        float3 _763 = _761.xyz * shrink;
        histSH[1].x = _763.x;
        histSH[1].y = _763.y;
        histSH[1].z = _763.z;
        float4 _773 = histSH[2];
        float3 _775 = _773.xyz * shrink;
        histSH[2].x = _775.x;
        histSH[2].y = _775.y;
        histSH[2].z = _775.z;
        float4 _784 = histSH[3];
        float3 _786 = _784.xyz * shrink;
        histSH[3].x = _786.x;
        histSH[3].y = _786.y;
        histSH[3].z = _786.z;
    }
    float alpha = clamp(params_baseAlpha, 0.0f, 1.0f);
    float4 _828;
    for (int b_5 = 0; b_5 < 4; b_5++)
    {
        float4 cur_2 = asfloat(currentSH.Load4((base_1 + uint(b_5)) * 16 + 0));
        float3 blended = lerp(histSH[b_5].xyz, cur_2.xyz, alpha.xxx);
        if (b_5 == 0)
        {
            _828 = float4(blended, probeConfidence);
        }
        else
        {
            _828 = float4(blended, cur_2.w);
        }
        outputSH.Store4((base_1 + uint(b_5)) * 16 + 0, asuint(_828));
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
