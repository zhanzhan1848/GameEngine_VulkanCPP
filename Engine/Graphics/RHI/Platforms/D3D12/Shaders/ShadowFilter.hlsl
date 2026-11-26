// Vulkan reference: ShadowFilter.comp (main).
#include "Common/TextureDimensions.hlsli"

static const float2 _159[16] = { float2(-0.5274373f, -0.725906f), float2(0.8218882f, -0.1694079f), float2(-0.7959683f, 0.5603005f), float2(0.5697909f, 0.7113788f), float2(-0.1583544f, -0.8058264f), float2(0.7826165f, 0.5406202f), float2(-0.6480213f, -0.1694079f), float2(0.55f, -0.45f), float2(-0.3f, 0.7f), float2(0.7f, -0.7f), float2(-0.7f, 0.3f), float2(0.3f, 0.8f), float2(-0.6f, -0.5f), float2(0.85f, 0.4f), float2(-0.2f, 0.9f), float2(0.4f, -0.3f) };

cbuffer Params : register(b0, space0)
{
    row_major float4x4 inv_view_proj : packoffset(c0);
    row_major float4x4 shadow_vp[4] : packoffset(c4);
    float4 light_dir : packoffset(c20);
    float2 texel_size : packoffset(c21);
    float2 depth_texel_size : packoffset(c21.z);
    uint shadow_quality : packoffset(c22);
    uint render_width : packoffset(c22.y);
    uint render_height : packoffset(c22.z);
    float sun_tan_half_angle : packoffset(c22.w);
    float4 cascade_params[4] : packoffset(c23);
};

SamplerState linearSampler : register(s6, space0);
RWTexture2D<unorm float> visibilityOut : register(u3, space0);
Texture2D<float4> depthTex : register(t5, space0);
Texture2D<float4> normalTex : register(t4, space0);
Texture2D<float4> shadowMap0 : register(t1, space0);
Texture2D<float4> shadowMap1 : register(t2, space0);
Texture2D<float4> shadowMap2 : register(t7, space0);
Texture2D<float4> shadowMap3 : register(t8, space0);
RWTexture2D<unorm float> translucentOut : register(u9, space0);

struct StageInput
{
    uint3 dispatchThreadID : SV_DispatchThreadID;
};

float3 cascadeNormalOffset(uint c, float3 N, float slope, float ndl) {
    float k = ndl > 0.0 ? min(1.0 + slope, 4.0) : (1.0 + 2.0 * min(slope, 4.0));
    return N * (cascade_params[c].x * k);
}

float cascadeBiasNDC(uint c, float slope, float ndl) {
    float texelWorld = cascade_params[c].x;
    float spanW = max(cascade_params[c].y, 1e-3);
    float s = ndl > 0.0 ? min(slope, 3.0) : min(slope, 4.0);
    return texelWorld * (0.5 + 2.0 * s) / spanW;
}

float3 projectToCascade(float4x4 vp, float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0f), vp);
    float3 sc = clip.xyz / max(clip.w, 1e-06f).xxx;
    sc.x = (sc.x * 0.5f) + 0.5f;
    sc.y = 0.5f - (sc.y * 0.5f);
    return sc;
}

bool scInBounds(float3 sc) {
    return sc.x >= 0.0 && sc.x <= 1.0 && sc.y >= 0.0 && sc.y <= 1.0;
}

float physicalLightSizeUV(uint c, float ndcZ) {
    float span = max(cascade_params[c].y, 1e-3);
    float worldWidth = max(cascade_params[c].z * 2.0, 1e-3);
    return ndcZ * span * sun_tan_half_angle / worldWidth;
}

float pcssLightSizeUV(uint c, float3 sc, float2 texSize) {
    if (sun_tan_half_angle <= 0.0) return 0.012;
    return clamp(physicalLightSizeUV(c, sc.z), texSize.x, 0.05);
}

float pcfShadow(Texture2D<float4> sm, float3 sc, float2 screenPos, float filterRadius, float bias)
{
    float shadow = 0.0f;
    float _noise = frac(0.26595822f * frac(dot(screenPos, float2(0.7548777f, 0.5698403f))));
    float angle = _noise * 6.2831855f;
    float sa = sin(angle);
    float ca = cos(angle);
    for (int i = 0; i < 8; i++)
    {
        float2 offset = _159[i];
        float2 rotated = float2((offset.x * ca) - (offset.y * sa), (offset.x * sa) + (offset.y * ca));
        float2 uv = sc.xy + (rotated * filterRadius);
        float d = sm.SampleLevel(linearSampler, uv, 0.0f).x;
        shadow += (((sc.z - bias) > d) ? 0.0f : 1.0f);
    }
    return shadow / 8.0f;
}

float pcssShadow(Texture2D<float4> sm, float3 sc, float2 screenPos, float2 texSize, float bias, float lightSizeUV)
{
    float blockerSum = 0.0f;
    float blockerCount = 0.0f;
    float _noise = frac(0.26595822f * frac(dot(screenPos, float2(0.7548777f, 0.5698403f))));
    float angle = _noise * 6.2831855f;
    float sa = sin(angle);
    float ca = cos(angle);
    for (int i = 0; i < 8; i++)
    {
        float2 offset = _159[i];
        float2 rotated = float2((offset.x * ca) - (offset.y * sa), (offset.x * sa) + (offset.y * ca));
        float2 uv = sc.xy + (rotated * lightSizeUV);
        float d = sm.SampleLevel(linearSampler, uv, 0.0f).x;
        if (d < (sc.z - bias))
        {
            blockerSum += d;
            blockerCount += 1.0f;
        }
    }
    if (blockerCount < 0.5f)
    {
        return 1.0f;
    }
    float penumbraRatio = (sc.z - (blockerSum / blockerCount)) / max(sc.z, 0.0001f);
    float filterRadius = clamp(penumbraRatio * lightSizeUV, texSize.x * 2.0f, lightSizeUV * 4.0f);
    float shadow = 0.0f;
    for (int i_1 = 0; i_1 < 16; i_1++)
    {
        float2 offset_1 = _159[i_1];
        float2 rotated_1 = float2((offset_1.x * ca) - (offset_1.y * sa), (offset_1.x * sa) + (offset_1.y * ca));
        float2 uv_1 = sc.xy + (rotated_1 * filterRadius);
        float d_1 = sm.SampleLevel(linearSampler, uv_1, 0.0f).x;
        shadow += (((sc.z - bias) > d_1) ? 0.0f : 1.0f);
    }
    return shadow / 16.0f;
}

float sampleShadowMap(Texture2D<float4> sm, float3 sc, float2 screenPos, float2 texSize, float bias, uint quality, float lightSizeUV)
{
    if (quality == 0u)
    {
        float d = sm.SampleLevel(linearSampler, sc.xy, 0.0f).x;
        return ((sc.z - bias) > d) ? 0.0f : 1.0f;
    }
    else
    {
        if (quality == 1u)
        {
            float3 param = sc;
            float2 param_1 = screenPos;
            float param_2 = 2.0f * texSize.x;
            float param_3 = bias;
            return pcfShadow(sm, param, param_1, param_2, param_3);
        }
        else
        {
            float3 param_4 = sc;
            float2 param_5 = screenPos;
            float2 param_6 = texSize;
            float param_7 = bias;
            float param_8 = lightSizeUV;
            return pcssShadow(sm, param_4, param_5, param_6, param_7, param_8);
        }
    }
}

[numthreads(8, 8, 1)]
void main(StageInput input)
{
    uint2 gid = input.dispatchThreadID.xy;

    uint2 outSize = uint2(int2(imageDimensions(visibilityOut)));
    if (any(bool2(gid.x >= outSize.x, gid.y >= outSize.y)))
    {
        return;
    }
    float2 fullUV = ((float2(gid) * 2.0f) + 1.0f.xx) / float2(float(render_width), float(render_height));
    if (shadow_quality == 255u)
    {
        visibilityOut[int2(gid)] = 1.0f.x;
        return;
    }
    float depth = depthTex.SampleLevel(linearSampler, fullUV, 0.0f).x;
    if (depth >= 1.0f)
    {
        visibilityOut[int2(gid)] = 1.0f.x;
        return;
    }
    float2 ndc = float2((fullUV.x * 2.0f) - 1.0f, 1.0f - (fullUV.y * 2.0f));
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos4 = mul(clipPos, inv_view_proj);
    float3 worldPos = worldPos4.xyz / max(worldPos4.w, 1e-06f).xxx;
    float3 N = (normalTex.SampleLevel(linearSampler, fullUV, 0.0f).xyz * 2.0f) - 1.0f.xxx;
    if (dot(N, N) < 0.0001f)
    {
        N = float3(0.0f, -1.0f, 0.0f);
    }
    N = normalize(N);
    float3 LtoLight = -normalize(light_dir.xyz);
    float NdotL = dot(N, LtoLight);
    float slope = clamp(sqrt(max(0.0f, 1.0f - (NdotL * NdotL))) / max(abs(NdotL), 0.25f), 0.0f, 4.0f);
    float2 screenPos = float2(gid);
    float visibility = 1.0f;
    float transmission = 0.0f;
    uint param = 0u;
    float3 param_1 = N;
    float param_2 = slope;
    float param_3 = NdotL;
    float3 offset0 = cascadeNormalOffset(param, param_1, param_2, param_3);
    uint param_4 = 0u;
    float param_5 = slope;
    float param_6 = NdotL;
    float bias0 = cascadeBiasNDC(param_4, param_5, param_6);
    float4x4 param_7 = shadow_vp[0];
    float3 param_8 = worldPos + offset0;
    float3 sc0 = projectToCascade(param_7, param_8);
    if (((shadow_quality >= 3u) && (shadow_quality <= 7u)))
    {
        float outV = 0.0f;
        if (shadow_quality == 7u)
        {
            outV = clamp((NdotL * 0.5f) + 0.5f, 0.0f, 1.0f);
        }
        else
        {
            if (shadow_quality == 4u)
            {
                float4x4 param_9 = shadow_vp[0];
                float3 param_10 = worldPos;
                float3 raw0 = projectToCascade(param_9, param_10);
                outV = clamp(raw0.z, 0.0f, 1.0f);
            }
            else
            {
                if (shadow_quality == 5u)
                {
                    if ((((scInBounds(sc0)) && (sc0.z >= 0.0f)) && (sc0.z <= 1.0f)))
                    {
                        float d = shadowMap0.SampleLevel(linearSampler, sc0.xy, 0.0f).x;
                        outV = ((sc0.z - bias0) > d) ? 0.0f : 1.0f;
                    }
                }
                else
                {
                    if (shadow_quality == 6u)
                    {
                        if ((((scInBounds(sc0)) && (sc0.z >= 0.0f)) && (sc0.z <= 1.0f)))
                        {
                            float cnt = 0.0f;
                            for (int i = 0; i < 8; i++)
                            {
                                float2 uv = sc0.xy + (_159[i] * 0.05f);
                                float d_1 = shadowMap0.SampleLevel(linearSampler, uv, 0.0f).x;
                                if (d_1 < (sc0.z - bias0))
                                {
                                    cnt += 1.0f;
                                }
                            }
                            outV = clamp((cnt / 8.0f) * 4.0f, 0.0f, 1.0f);
                        }
                    }
                    else
                    {
                        if ((((scInBounds(sc0)) && (sc0.z >= 0.0f)) && (sc0.z <= 1.0f)))
                        {
                            outV = shadowMap0.SampleLevel(linearSampler, sc0.xy, 0.0f).x;
                        }
                    }
                }
            }
        }
        visibilityOut[int2(gid)] = outV.x;
        return;
    }
    uint param_14 = 0u;
    float3 param_15 = N;
    float param_16 = slope;
    float param_17 = NdotL;
    float3 offset = cascadeNormalOffset(param_14, param_15, param_16, param_17);
    uint param_18 = 0u;
    float param_19 = slope;
    float bias = cascadeBiasNDC(param_18, param_19, NdotL);
    float4x4 param_21 = shadow_vp[0];
    float3 param_22 = worldPos + offset;
    float3 sc = projectToCascade(param_21, param_22);
    bool _998 = ((scInBounds(sc)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f);
    bool inCascade = _998;
    if (inCascade)
    {
        uint param_24 = 0u;
        float3 param_25 = sc;
        float2 param_26 = texel_size;
        float3 param_27 = sc;
        float2 param_28 = screenPos;
        float2 param_29 = texel_size;
        float param_30 = bias;
        uint param_31 = shadow_quality;
        visibility = sampleShadowMap(shadowMap0, param_27, param_28, param_29, param_30, param_31, pcssLightSizeUV(param_24, param_25, param_26));
        float occ = shadowMap0.SampleLevel(linearSampler, sc.xy, 0.0f).x;
        float thW = max((sc.z - bias) - occ, 0.0f) * cascade_params[0].y;
        transmission = (exp((-thW) * 1.5f) * max(-NdotL, 0.0f)) * (1.0f - visibility);
    }
    else
    {
        uint param_33 = 1u;
        float3 param_34 = N;
        float param_35 = slope;
        float param_36 = NdotL;
        offset = cascadeNormalOffset(param_33, param_34, param_35, param_36);
        uint param_37 = 1u;
        float param_38 = slope;
        bias = cascadeBiasNDC(param_37, param_38, NdotL);
        float4x4 param_40 = shadow_vp[1];
        float3 param_41 = worldPos + offset;
        sc = projectToCascade(param_40, param_41);
        bool _1093 = ((scInBounds(sc)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f);
        inCascade = _1093;
        if (inCascade)
        {
            uint param_43 = 1u;
            float3 param_44 = sc;
            float2 param_45 = texel_size;
            float3 param_46 = sc;
            float2 param_47 = screenPos;
            float2 param_48 = texel_size;
            float param_49 = bias;
            uint param_50 = shadow_quality;
            visibility = sampleShadowMap(shadowMap1, param_46, param_47, param_48, param_49, param_50, pcssLightSizeUV(param_43, param_44, param_45));
            float occ1 = shadowMap1.SampleLevel(linearSampler, sc.xy, 0.0f).x;
            float thW1 = max((sc.z - bias) - occ1, 0.0f) * cascade_params[1].y;
            transmission = (exp((-thW1) * 1.5f) * max(-NdotL, 0.0f)) * (1.0f - visibility);
        }
        else
        {
            uint param_52 = 2u;
            float3 param_53 = N;
            float param_54 = slope;
            float param_55 = NdotL;
            offset = cascadeNormalOffset(param_52, param_53, param_54, param_55);
            uint param_56 = 2u;
            float param_57 = slope;
            bias = cascadeBiasNDC(param_56, param_57, NdotL);
            float4x4 param_59 = shadow_vp[2];
            float3 param_60 = worldPos + offset;
            sc = projectToCascade(param_59, param_60);
            bool _1186 = ((scInBounds(sc)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f);
            inCascade = _1186;
            if (inCascade)
            {
                uint param_62 = 2u;
                float3 param_63 = sc;
                float2 param_64 = texel_size;
                float3 param_65 = sc;
                float2 param_66 = screenPos;
                float2 param_67 = texel_size;
                float param_68 = bias;
                uint param_69 = shadow_quality;
                visibility = sampleShadowMap(shadowMap2, param_65, param_66, param_67, param_68, param_69, pcssLightSizeUV(param_62, param_63, param_64));
                float occ2 = shadowMap2.SampleLevel(linearSampler, sc.xy, 0.0f).x;
                float thW2 = max((sc.z - bias) - occ2, 0.0f) * cascade_params[2].y;
                transmission = (exp((-thW2) * 1.5f) * max(-NdotL, 0.0f)) * (1.0f - visibility);
            }
            else
            {
                uint param_71 = 3u;
                float3 param_72 = N;
                float param_73 = slope;
                float param_74 = NdotL;
                offset = cascadeNormalOffset(param_71, param_72, param_73, param_74);
                uint param_75 = 3u;
                float param_76 = slope;
                bias = cascadeBiasNDC(param_75, param_76, NdotL);
                float4x4 param_78 = shadow_vp[3];
                float3 param_79 = worldPos + offset;
                sc = projectToCascade(param_78, param_79);
                if ((((scInBounds(sc)) && (sc.z >= 0.0f)) && (sc.z <= 1.0f)))
                {
                    uint param_81 = 3u;
                    float3 param_82 = sc;
                    float2 param_83 = texel_size;
                    float3 param_84 = sc;
                    float2 param_85 = screenPos;
                    float2 param_86 = texel_size;
                    float param_87 = bias;
                    uint param_88 = shadow_quality;
                    visibility = sampleShadowMap(shadowMap3, param_84, param_85, param_86, param_87, param_88, pcssLightSizeUV(param_81, param_82, param_83));
                    float occ3 = shadowMap3.SampleLevel(linearSampler, sc.xy, 0.0f).x;
                    float thW3 = max((sc.z - bias) - occ3, 0.0f) * cascade_params[3].y;
                    transmission = (exp((-thW3) * 1.5f) * max(-NdotL, 0.0f)) * (1.0f - visibility);
                }
            }
        }
    }
    visibilityOut[int2(gid)] = visibility.x;
    translucentOut[int2(gid)] = clamp(transmission, 0.0f, 1.0f).x;
}
