static const uint3 gl_WorkGroupSize = uint3(8u, 8u, 1u);

static const float2 _159[16] = { float2(-0.52743732929229736328125f, -0.72590601444244384765625f), float2(0.8218882083892822265625f, -0.169407904148101806640625f), float2(-0.7959682941436767578125f, 0.560300529003143310546875f), float2(0.569790899753570556640625f, 0.7113788127899169921875f), float2(-0.15835440158843994140625f, -0.8058264255523681640625f), float2(0.78261649608612060546875f, 0.54062020778656005859375f), float2(-0.648021280765533447265625f, -0.169407904148101806640625f), float2(0.550000011920928955078125f, -0.449999988079071044921875f), float2(-0.300000011920928955078125f, 0.699999988079071044921875f), float2(0.699999988079071044921875f, -0.699999988079071044921875f), float2(-0.699999988079071044921875f, 0.300000011920928955078125f), float2(0.300000011920928955078125f, 0.800000011920928955078125f), float2(-0.60000002384185791015625f, -0.5f), float2(0.85000002384185791015625f, 0.4000000059604644775390625f), float2(-0.20000000298023223876953125f, 0.89999997615814208984375f), float2(0.4000000059604644775390625f, -0.300000011920928955078125f) };

cbuffer Params : register(b0, space0)
{
    row_major float4x4 _455_inv_view_proj : packoffset(c0);
    row_major float4x4 _455_shadow_vp[4] : packoffset(c4);
    float4 _455_light_dir : packoffset(c20);
    float2 _455_texel_size : packoffset(c21);
    float2 _455_depth_texel_size : packoffset(c21.z);
    uint _455_shadow_quality : packoffset(c22);
    uint _455_render_width : packoffset(c22.y);
    uint _455_render_height : packoffset(c22.z);
    float _455_sun_tan_half_angle : packoffset(c22.w);
    float4 _455_cascade_params[4] : packoffset(c23);
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

static uint3 gl_GlobalInvocationID;
struct SPIRV_Cross_Input
{
    uint3 gl_GlobalInvocationID : SV_DispatchThreadID;
};

uint2 spvImageSize(RWTexture2D<unorm float> Tex, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(ret.x, ret.y);
    Param = 0u;
    return ret;
}

float3 cascadeNormalOffset(uint c, float3 N, float slope, float ndl)
{
    float _587;
    if (ndl > 0.0f)
    {
        _587 = min(1.0f + slope, 4.0f);
    }
    else
    {
        _587 = 1.0f + (2.0f * min(slope, 4.0f));
    }
    float k = _587;
    return N * (_455_cascade_params[c].x * k);
}

float cascadeBiasNDC(uint c, float slope, float ndl)
{
    float texelWorld = _455_cascade_params[c].x;
    float spanW = max(_455_cascade_params[c].y, 0.001000000047497451305389404296875f);
    float _565;
    if (ndl > 0.0f)
    {
        _565 = min(slope, 3.0f);
    }
    else
    {
        _565 = min(slope, 4.0f);
    }
    float s = _565;
    return (texelWorld * (0.5f + (2.0f * s))) / spanW;
}

float3 projectToCascade(float4x4 vp, float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0f), vp);
    float3 sc = clip.xyz / max(clip.w, 9.9999999747524270787835121154785e-07f).xxx;
    sc.x = (sc.x * 0.5f) + 0.5f;
    sc.y = 0.5f - (sc.y * 0.5f);
    return sc;
}

bool scInBounds(float3 sc)
{
    bool _532 = sc.x >= 0.0f;
    bool _538;
    if (_532)
    {
        _538 = sc.x <= 1.0f;
    }
    else
    {
        _538 = _532;
    }
    bool _544;
    if (_538)
    {
        _544 = sc.y >= 0.0f;
    }
    else
    {
        _544 = _538;
    }
    bool _550;
    if (_544)
    {
        _550 = sc.y <= 1.0f;
    }
    else
    {
        _550 = _544;
    }
    return _550;
}

float physicalLightSizeUV(uint c, float ndcZ)
{
    float span = max(_455_cascade_params[c].y, 0.001000000047497451305389404296875f);
    float worldWidth = max(_455_cascade_params[c].z * 2.0f, 0.001000000047497451305389404296875f);
    return ((ndcZ * span) * _455_sun_tan_half_angle) / worldWidth;
}

float pcssLightSizeUV(uint c, float3 sc, float2 texSize)
{
    if (_455_sun_tan_half_angle <= 0.0f)
    {
        return 0.01200000010430812835693359375f;
    }
    uint param = c;
    float param_1 = sc.z;
    return clamp(physicalLightSizeUV(param, param_1), texSize.x, 0.0500000007450580596923828125f);
}

float pcfShadow(Texture2D<float4> sm, float3 sc, float2 screenPos, float filterRadius, float bias)
{
    float shadow = 0.0f;
    float _noise = frac(0.2659582197666168212890625f * frac(dot(screenPos, float2(0.75487768650054931640625f, 0.56984031200408935546875f))));
    float angle = _noise * 6.283185482025146484375f;
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
    float _noise = frac(0.2659582197666168212890625f * frac(dot(screenPos, float2(0.75487768650054931640625f, 0.56984031200408935546875f))));
    float angle = _noise * 6.283185482025146484375f;
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
    float penumbraRatio = (sc.z - (blockerSum / blockerCount)) / max(sc.z, 9.9999997473787516355514526367188e-05f);
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

void comp_main()
{
    uint2 gid = gl_GlobalInvocationID.xy;
    uint _622_dummy_parameter;
    uint2 outSize = uint2(int2(spvImageSize(visibilityOut, _622_dummy_parameter)));
    if (any(bool2(gid.x >= outSize.x, gid.y >= outSize.y)))
    {
        return;
    }
    float2 fullUV = ((float2(gid) * 2.0f) + 1.0f.xx) / float2(float(_455_render_width), float(_455_render_height));
    if (_455_shadow_quality == 255u)
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
    float4 worldPos4 = mul(clipPos, _455_inv_view_proj);
    float3 worldPos = worldPos4.xyz / max(worldPos4.w, 9.9999999747524270787835121154785e-07f).xxx;
    float3 N = (normalTex.SampleLevel(linearSampler, fullUV, 0.0f).xyz * 2.0f) - 1.0f.xxx;
    if (dot(N, N) < 9.9999997473787516355514526367188e-05f)
    {
        N = float3(0.0f, -1.0f, 0.0f);
    }
    N = normalize(N);
    float3 LtoLight = -normalize(_455_light_dir.xyz);
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
    float4x4 param_7 = _455_shadow_vp[0];
    float3 param_8 = worldPos + offset0;
    float3 sc0 = projectToCascade(param_7, param_8);
    bool _785 = _455_shadow_quality >= 3u;
    bool _792;
    if (_785)
    {
        _792 = _455_shadow_quality <= 7u;
    }
    else
    {
        _792 = _785;
    }
    if (_792)
    {
        float outV = 0.0f;
        if (_455_shadow_quality == 7u)
        {
            outV = clamp((NdotL * 0.5f) + 0.5f, 0.0f, 1.0f);
        }
        else
        {
            if (_455_shadow_quality == 4u)
            {
                float4x4 param_9 = _455_shadow_vp[0];
                float3 param_10 = worldPos;
                float3 raw0 = projectToCascade(param_9, param_10);
                outV = clamp(raw0.z, 0.0f, 1.0f);
            }
            else
            {
                if (_455_shadow_quality == 5u)
                {
                    float3 param_11 = sc0;
                    bool _830 = scInBounds(param_11);
                    bool _836;
                    if (_830)
                    {
                        _836 = sc0.z >= 0.0f;
                    }
                    else
                    {
                        _836 = _830;
                    }
                    bool _842;
                    if (_836)
                    {
                        _842 = sc0.z <= 1.0f;
                    }
                    else
                    {
                        _842 = _836;
                    }
                    if (_842)
                    {
                        float d = shadowMap0.SampleLevel(linearSampler, sc0.xy, 0.0f).x;
                        outV = ((sc0.z - bias0) > d) ? 0.0f : 1.0f;
                    }
                }
                else
                {
                    if (_455_shadow_quality == 6u)
                    {
                        float3 param_12 = sc0;
                        bool _870 = scInBounds(param_12);
                        bool _876;
                        if (_870)
                        {
                            _876 = sc0.z >= 0.0f;
                        }
                        else
                        {
                            _876 = _870;
                        }
                        bool _882;
                        if (_876)
                        {
                            _882 = sc0.z <= 1.0f;
                        }
                        else
                        {
                            _882 = _876;
                        }
                        if (_882)
                        {
                            float cnt = 0.0f;
                            for (int i = 0; i < 8; i++)
                            {
                                float2 uv = sc0.xy + (_159[i] * 0.0500000007450580596923828125f);
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
                        float3 param_13 = sc0;
                        bool _929 = scInBounds(param_13);
                        bool _935;
                        if (_929)
                        {
                            _935 = sc0.z >= 0.0f;
                        }
                        else
                        {
                            _935 = _929;
                        }
                        bool _941;
                        if (_935)
                        {
                            _941 = sc0.z <= 1.0f;
                        }
                        else
                        {
                            _941 = _935;
                        }
                        if (_941)
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
    float param_20 = NdotL;
    float bias = cascadeBiasNDC(param_18, param_19, param_20);
    float4x4 param_21 = _455_shadow_vp[0];
    float3 param_22 = worldPos + offset;
    float3 sc = projectToCascade(param_21, param_22);
    float3 param_23 = sc;
    bool _986 = scInBounds(param_23);
    bool _992;
    if (_986)
    {
        _992 = sc.z >= 0.0f;
    }
    else
    {
        _992 = _986;
    }
    bool _998;
    if (_992)
    {
        _998 = sc.z <= 1.0f;
    }
    else
    {
        _998 = _992;
    }
    bool inCascade = _998;
    if (inCascade)
    {
        uint param_24 = 0u;
        float3 param_25 = sc;
        float2 param_26 = _455_texel_size;
        float3 param_27 = sc;
        float2 param_28 = screenPos;
        float2 param_29 = _455_texel_size;
        float param_30 = bias;
        uint param_31 = _455_shadow_quality;
        float param_32 = pcssLightSizeUV(param_24, param_25, param_26);
        visibility = sampleShadowMap(shadowMap0, param_27, param_28, param_29, param_30, param_31, param_32);
        float occ = shadowMap0.SampleLevel(linearSampler, sc.xy, 0.0f).x;
        float thW = max((sc.z - bias) - occ, 0.0f) * _455_cascade_params[0].y;
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
        float param_39 = NdotL;
        bias = cascadeBiasNDC(param_37, param_38, param_39);
        float4x4 param_40 = _455_shadow_vp[1];
        float3 param_41 = worldPos + offset;
        sc = projectToCascade(param_40, param_41);
        float3 param_42 = sc;
        bool _1081 = scInBounds(param_42);
        bool _1087;
        if (_1081)
        {
            _1087 = sc.z >= 0.0f;
        }
        else
        {
            _1087 = _1081;
        }
        bool _1093;
        if (_1087)
        {
            _1093 = sc.z <= 1.0f;
        }
        else
        {
            _1093 = _1087;
        }
        inCascade = _1093;
        if (inCascade)
        {
            uint param_43 = 1u;
            float3 param_44 = sc;
            float2 param_45 = _455_texel_size;
            float3 param_46 = sc;
            float2 param_47 = screenPos;
            float2 param_48 = _455_texel_size;
            float param_49 = bias;
            uint param_50 = _455_shadow_quality;
            float param_51 = pcssLightSizeUV(param_43, param_44, param_45);
            visibility = sampleShadowMap(shadowMap1, param_46, param_47, param_48, param_49, param_50, param_51);
            float occ1 = shadowMap1.SampleLevel(linearSampler, sc.xy, 0.0f).x;
            float thW1 = max((sc.z - bias) - occ1, 0.0f) * _455_cascade_params[1].y;
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
            float param_58 = NdotL;
            bias = cascadeBiasNDC(param_56, param_57, param_58);
            float4x4 param_59 = _455_shadow_vp[2];
            float3 param_60 = worldPos + offset;
            sc = projectToCascade(param_59, param_60);
            float3 param_61 = sc;
            bool _1174 = scInBounds(param_61);
            bool _1180;
            if (_1174)
            {
                _1180 = sc.z >= 0.0f;
            }
            else
            {
                _1180 = _1174;
            }
            bool _1186;
            if (_1180)
            {
                _1186 = sc.z <= 1.0f;
            }
            else
            {
                _1186 = _1180;
            }
            inCascade = _1186;
            if (inCascade)
            {
                uint param_62 = 2u;
                float3 param_63 = sc;
                float2 param_64 = _455_texel_size;
                float3 param_65 = sc;
                float2 param_66 = screenPos;
                float2 param_67 = _455_texel_size;
                float param_68 = bias;
                uint param_69 = _455_shadow_quality;
                float param_70 = pcssLightSizeUV(param_62, param_63, param_64);
                visibility = sampleShadowMap(shadowMap2, param_65, param_66, param_67, param_68, param_69, param_70);
                float occ2 = shadowMap2.SampleLevel(linearSampler, sc.xy, 0.0f).x;
                float thW2 = max((sc.z - bias) - occ2, 0.0f) * _455_cascade_params[2].y;
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
                float param_77 = NdotL;
                bias = cascadeBiasNDC(param_75, param_76, param_77);
                float4x4 param_78 = _455_shadow_vp[3];
                float3 param_79 = worldPos + offset;
                sc = projectToCascade(param_78, param_79);
                float3 param_80 = sc;
                bool _1267 = scInBounds(param_80);
                bool _1273;
                if (_1267)
                {
                    _1273 = sc.z >= 0.0f;
                }
                else
                {
                    _1273 = _1267;
                }
                bool _1279;
                if (_1273)
                {
                    _1279 = sc.z <= 1.0f;
                }
                else
                {
                    _1279 = _1273;
                }
                if (_1279)
                {
                    uint param_81 = 3u;
                    float3 param_82 = sc;
                    float2 param_83 = _455_texel_size;
                    float3 param_84 = sc;
                    float2 param_85 = screenPos;
                    float2 param_86 = _455_texel_size;
                    float param_87 = bias;
                    uint param_88 = _455_shadow_quality;
                    float param_89 = pcssLightSizeUV(param_81, param_82, param_83);
                    visibility = sampleShadowMap(shadowMap3, param_84, param_85, param_86, param_87, param_88, param_89);
                    float occ3 = shadowMap3.SampleLevel(linearSampler, sc.xy, 0.0f).x;
                    float thW3 = max((sc.z - bias) - occ3, 0.0f) * _455_cascade_params[3].y;
                    transmission = (exp((-thW3) * 1.5f) * max(-NdotL, 0.0f)) * (1.0f - visibility);
                }
            }
        }
    }
    visibilityOut[int2(gid)] = visibility.x;
    translucentOut[int2(gid)] = clamp(transmission, 0.0f, 1.0f).x;
}

[numthreads(8, 8, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_GlobalInvocationID = stage_input.gl_GlobalInvocationID;
    comp_main();
}
