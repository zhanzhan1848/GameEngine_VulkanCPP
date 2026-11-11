cbuffer SceneData : register(b1, space0)
{
    row_major float4x4 _364_model : packoffset(c0);
    float4 _364_lightPos : packoffset(c4);
    float4 _364_lightColor : packoffset(c5);
    float4 _364_reflectionPlane : packoffset(c6);
    float4 _364_reflectionPlane2 : packoffset(c7);
    float4 _364_reflectionPlane3 : packoffset(c8);
    row_major float4x4 _364_previousModel : packoffset(c9);
    float2 _364_jitter : packoffset(c13);
    float2 _364_previousJitter : packoffset(c13.z);
    float _364_time : packoffset(c14);
    float _364_timePad : packoffset(c14.y);
    float4 _364_viewPos : packoffset(c15);
    row_major float4x4 _364_shadowMatrix0 : packoffset(c16);
    row_major float4x4 _364_shadowMatrix1 : packoffset(c20);
    row_major float4x4 _364_shadowMatrix2 : packoffset(c24);
    row_major float4x4 _364_shadowMatrix3 : packoffset(c28);
    float4 _364_shadowParams : packoffset(c32);
    float4 _364_ambientLight : packoffset(c33);
};

cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 _399_viewProjection : packoffset(c0);
    row_major float4x4 _399_invViewProjection : packoffset(c4);
};

ByteAddressBuffer _1253 : register(t18, space0);
cbuffer ProbeParamsUBO : register(b19, space0)
{
    float4 _1256_probeOrigin_spacing : packoffset(c0);
    float4 _1256_probeCounts : packoffset(c1);
};

TextureCube<float4> prefilterMap : register(t11, space0);
SamplerState defaultSampler : register(s8, space0);
Texture2D<float4> albedoTex : register(t2, space0);
Texture2D<float4> normalTex : register(t3, space0);
Texture2D<float4> ormTex : register(t4, space0);
Texture2D<float4> depthTex : register(t5, space0);
Texture2D<float4> ssaoTex : register(t7, space0);
Texture2D<float4> shadowMoments0 : register(t13, space0);
Texture2D<float4> shadowMoments1 : register(t14, space0);
Texture2D<float4> shadowMoments2 : register(t15, space0);
Texture2D<float4> shadowMoments3 : register(t16, space0);
Texture2D<float4> shadowVisTex : register(t6, space0);
Texture2D<float4> brdfLUT : register(t12, space0);
Texture2D<float4> translucentTex : register(t17, space0);
TextureCube<float4> irradianceMap : register(t10, space0);
Texture2D<float4> fallbackTex : register(t9, space0);

static float4 outEnvironmentSpecular;
static float2 inUv;
static float4 outColor;

struct SPIRV_Cross_Input
{
    float2 inUv : TEXCOORD0;
};

struct SPIRV_Cross_Output
{
    float4 outColor : SV_Target0;
    float4 outEnvironmentSpecular : SV_Target1;
};

uint2 spvTextureSize(TextureCube<float4> Tex, uint Level, out uint Param)
{
    uint2 ret;
    Tex.GetDimensions(Level, ret.x, ret.y, Param);
    return ret;
}

float3 EvaluateSkyBase(float3 dir)
{
    float up = clamp(dir.y, 0.0f, 1.0f);
    float3 horizonCol = float3(4.400000095367431640625f, 4.19999980926513671875f, 3.7999999523162841796875f);
    float3 zenithCol = float3(5.5f, 5.7600002288818359375f, 6.0f);
    return lerp(horizonCol, zenithCol, pow(up, 0.449999988079071044921875f).xxx);
}

float3 EvaluateSky(float3 dir, float3 sunDir, float3 sunLinearColor)
{
    float3 param = dir;
    float3 sky = EvaluateSkyBase(param);
    float sunAmount = pow(clamp(dot(dir, sunDir), 0.0f, 1.0f), 16.0f);
    float lum = dot(sunLinearColor, float3(0.2125999927520751953125f, 0.715200006961822509765625f, 0.072200000286102294921875f));
    float3 _111;
    if (lum > 9.9999997473787516355514526367188e-05f)
    {
        _111 = sunLinearColor * (0.86383998394012451171875f / lum);
    }
    else
    {
        _111 = float3(1.0f, 0.85000002384185791015625f, 0.60000002384185791015625f);
    }
    float3 halo = _111;
    sky += ((halo * sunAmount) * 1.2000000476837158203125f);
    return sky;
}

float DistributionGGX(float3 N, float3 H, float a)
{
    float a2 = ((a * a) * a) * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0f)) + 1.0f;
    denom = (3.1415927410125732421875f * denom) * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float k)
{
    float r = k + 1.0f;
    float a = (r * r) / 8.0f;
    return NdotV / ((NdotV * (1.0f - k)) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float k)
{
    float param = max(dot(N, V), 0.0f);
    float param_1 = k;
    float param_2 = max(dot(N, L), 0.0f);
    float param_3 = k;
    return GeometrySchlickGGX(param, param_1) * GeometrySchlickGGX(param_2, param_3);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + ((1.0f.xxx - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f));
}

float chebyshevVisibility(float2 moments, float t)
{
    if (t <= moments.x)
    {
        return 1.0f;
    }
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, 1.9999999494757503271102905273438e-05f);
    float d = t - moments.x;
    float pMax = variance / (variance + (d * d));
    return smoothstep(0.0500000007450580596923828125f, 1.0f, pMax);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + ((max((1.0f - roughness).xxx, F0) - F0) * pow(clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f));
}

float3 SamplePrefilteredEnvironment(float3 direction, float roughness)
{
    int _287;
    spvTextureSize(prefilterMap, 0u, _287);
    float maxLod = float(max((int(_287) - 1), 0));
    return prefilterMap.SampleLevel(defaultSampler, direction, clamp(roughness, 0.0f, 1.0f) * maxLod).xyz;
}

void frag_main()
{
    outEnvironmentSpecular = 0.0f.xxxx;
    float4 albedoV = albedoTex.Sample(defaultSampler, inUv);
    float3 albedo = albedoV.xyz;
    float3 normalEnc = normalTex.Sample(defaultSampler, inUv).xyz;
    float3 orm = ormTex.Sample(defaultSampler, inUv).xyz;
    float transmissionMask = ormTex.Sample(defaultSampler, inUv).w;
    float depth = depthTex.Sample(defaultSampler, inUv).x;
    if (depth >= 1.0f)
    {
        bool _371 = _364_shadowParams.z > 0.5f;
        bool _378;
        if (_371)
        {
            _378 = _364_shadowParams.z < 10.5f;
        }
        else
        {
            _378 = _371;
        }
        if (_378)
        {
            outColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
            return;
        }
        float2 skyNdc = float2((inUv.x * 2.0f) - 1.0f, 1.0f - (inUv.y * 2.0f));
        float4 pFar = mul(float4(skyNdc, 1.0f, 1.0f), _399_invViewProjection);
        float3 skyDir = normalize((pFar.xyz / max(pFar.w, 9.9999999747524270787835121154785e-07f).xxx) - _364_viewPos.xyz);
        float3 param = skyDir;
        float3 param_1 = normalize(_364_lightPos.xyz);
        float3 param_2 = _364_lightColor.xyz;
        float3 sky = EvaluateSky(param, param_1, param_2);
        outColor = float4(sky, 1.0f);
        return;
    }
    float2 ndcXY = float2((inUv.x * 2.0f) - 1.0f, 1.0f - (inUv.y * 2.0f));
    float4 ndc = float4(ndcXY, depth, 1.0f);
    float4 worldPosH = mul(ndc, _399_invViewProjection);
    float3 worldPos = worldPosH.xyz / max(worldPosH.w, 9.9999999747524270787835121154785e-07f).xxx;
    float3 N = normalize((normalEnc * 2.0f) - 1.0f.xxx);
    float4 ssao = ssaoTex.Sample(defaultSampler, inUv);
    float _490;
    if (ssao.x > 0.0f)
    {
        _490 = ssao.x;
    }
    else
    {
        _490 = 1.0f;
    }
    float ao = _490;
    float roughness = max(orm.y, 0.039999999105930328369140625f);
    float metallic = orm.z;
    if (_364_shadowParams.z >= 10.5f)
    {
        if (_364_shadowParams.z < 11.5f)
        {
            albedo = 0.5f.xxx;
        }
        else
        {
            if (_364_shadowParams.z < 12.5f)
            {
                N = float3(0.0f, 1.0f, 0.0f);
            }
            else
            {
                if (_364_shadowParams.z < 13.5f)
                {
                    roughness = 1.0f;
                }
            }
        }
    }
    float3 V = normalize(_364_viewPos.xyz - worldPos);
    float3 L = normalize(_364_lightPos.xyz);
    float3 H = normalize(V + L);
    float3 F0 = lerp(0.039999999105930328369140625f.xxx, albedo, metallic.xxx);
    float3 param_3 = N;
    float3 param_4 = H;
    float param_5 = roughness;
    float NDF = DistributionGGX(param_3, param_4, param_5);
    float3 param_6 = N;
    float3 param_7 = V;
    float3 param_8 = L;
    float param_9 = roughness;
    float G = GeometrySmith(param_6, param_7, param_8, param_9);
    float param_10 = max(dot(H, V), 0.0f);
    float3 param_11 = F0;
    float3 F = FresnelSchlick(param_10, param_11);
    float NdotV = max(dot(N, V), 0.0f);
    float NdotL = max(dot(N, L), 0.0f);
    float3 specular = (F * (NDF * G)) / (((4.0f * NdotV) * NdotL) + 0.001000000047497451305389404296875f).xxx;
    float3 kS = F;
    float3 kD = (1.0f.xxx - kS) * (1.0f - metallic);
    float3 diffuse = (kD * albedo) / 3.1415927410125732421875f.xxx;
    float shadowVisibility;
    if (_364_shadowParams.x > 0.75f)
    {
        shadowVisibility = 1.0f;
        float4 clip0 = mul(float4(worldPos, 1.0f), _364_shadowMatrix0);
        float3 sc0 = clip0.xyz / clip0.w.xxx;
        float2 uv0 = float2((sc0.x * 0.5f) + 0.5f, 0.5f - (sc0.y * 0.5f));
        bool _660 = uv0.x >= 0.0f;
        bool _666;
        if (_660)
        {
            _666 = uv0.x <= 1.0f;
        }
        else
        {
            _666 = _660;
        }
        bool _672;
        if (_666)
        {
            _672 = uv0.y >= 0.0f;
        }
        else
        {
            _672 = _666;
        }
        bool _678;
        if (_672)
        {
            _678 = uv0.y <= 1.0f;
        }
        else
        {
            _678 = _672;
        }
        bool _684;
        if (_678)
        {
            _684 = sc0.z >= 0.0f;
        }
        else
        {
            _684 = _678;
        }
        bool _690;
        if (_684)
        {
            _690 = sc0.z <= 1.0f;
        }
        else
        {
            _690 = _684;
        }
        bool inCascade0 = _690;
        if (inCascade0)
        {
            float2 moments = shadowMoments0.Sample(defaultSampler, uv0).xy;
            float2 param_12 = moments;
            float param_13 = sc0.z;
            shadowVisibility = chebyshevVisibility(param_12, param_13);
        }
        else
        {
            float4 clip1 = mul(float4(worldPos, 1.0f), _364_shadowMatrix1);
            float3 sc1 = clip1.xyz / clip1.w.xxx;
            float2 uv1 = float2((sc1.x * 0.5f) + 0.5f, 0.5f - (sc1.y * 0.5f));
            bool _739 = uv1.x >= 0.0f;
            bool _745;
            if (_739)
            {
                _745 = uv1.x <= 1.0f;
            }
            else
            {
                _745 = _739;
            }
            bool _751;
            if (_745)
            {
                _751 = uv1.y >= 0.0f;
            }
            else
            {
                _751 = _745;
            }
            bool _757;
            if (_751)
            {
                _757 = uv1.y <= 1.0f;
            }
            else
            {
                _757 = _751;
            }
            bool _763;
            if (_757)
            {
                _763 = sc1.z >= 0.0f;
            }
            else
            {
                _763 = _757;
            }
            bool _769;
            if (_763)
            {
                _769 = sc1.z <= 1.0f;
            }
            else
            {
                _769 = _763;
            }
            bool inCascade1 = _769;
            if (inCascade1)
            {
                float2 moments_1 = shadowMoments1.Sample(defaultSampler, uv1).xy;
                float2 param_14 = moments_1;
                float param_15 = sc1.z;
                shadowVisibility = chebyshevVisibility(param_14, param_15);
            }
            else
            {
                float4 clip2 = mul(float4(worldPos, 1.0f), _364_shadowMatrix2);
                float3 sc2 = clip2.xyz / clip2.w.xxx;
                float2 uv2 = float2((sc2.x * 0.5f) + 0.5f, 0.5f - (sc2.y * 0.5f));
                bool _818 = uv2.x >= 0.0f;
                bool _824;
                if (_818)
                {
                    _824 = uv2.x <= 1.0f;
                }
                else
                {
                    _824 = _818;
                }
                bool _830;
                if (_824)
                {
                    _830 = uv2.y >= 0.0f;
                }
                else
                {
                    _830 = _824;
                }
                bool _836;
                if (_830)
                {
                    _836 = uv2.y <= 1.0f;
                }
                else
                {
                    _836 = _830;
                }
                bool _842;
                if (_836)
                {
                    _842 = sc2.z >= 0.0f;
                }
                else
                {
                    _842 = _836;
                }
                bool _848;
                if (_842)
                {
                    _848 = sc2.z <= 1.0f;
                }
                else
                {
                    _848 = _842;
                }
                bool inCascade2 = _848;
                if (inCascade2)
                {
                    float2 moments_2 = shadowMoments2.Sample(defaultSampler, uv2).xy;
                    float2 param_16 = moments_2;
                    float param_17 = sc2.z;
                    shadowVisibility = chebyshevVisibility(param_16, param_17);
                }
                else
                {
                    float4 clip3 = mul(float4(worldPos, 1.0f), _364_shadowMatrix3);
                    float3 sc3 = clip3.xyz / clip3.w.xxx;
                    float2 uv3 = float2((sc3.x * 0.5f) + 0.5f, 0.5f - (sc3.y * 0.5f));
                    bool _896 = uv3.x >= 0.0f;
                    bool _902;
                    if (_896)
                    {
                        _902 = uv3.x <= 1.0f;
                    }
                    else
                    {
                        _902 = _896;
                    }
                    bool _908;
                    if (_902)
                    {
                        _908 = uv3.y >= 0.0f;
                    }
                    else
                    {
                        _908 = _902;
                    }
                    bool _914;
                    if (_908)
                    {
                        _914 = uv3.y <= 1.0f;
                    }
                    else
                    {
                        _914 = _908;
                    }
                    bool _920;
                    if (_914)
                    {
                        _920 = sc3.z >= 0.0f;
                    }
                    else
                    {
                        _920 = _914;
                    }
                    bool _926;
                    if (_920)
                    {
                        _926 = sc3.z <= 1.0f;
                    }
                    else
                    {
                        _926 = _920;
                    }
                    if (_926)
                    {
                        float2 moments_3 = shadowMoments3.Sample(defaultSampler, uv3).xy;
                        float2 param_18 = moments_3;
                        float param_19 = sc3.z;
                        shadowVisibility = chebyshevVisibility(param_18, param_19);
                    }
                }
            }
        }
    }
    else
    {
        shadowVisibility = shadowVisTex.Sample(defaultSampler, inUv).x;
    }
    bool _953 = _364_shadowParams.z > 0.5f;
    bool _959;
    if (_953)
    {
        _959 = _364_shadowParams.z < 10.5f;
    }
    else
    {
        _959 = _953;
    }
    if (_959)
    {
        float3 diagnostic = normalEnc;
        if (_364_shadowParams.z == 2.0f)
        {
            diagnostic = NdotL.xxx;
        }
        if (_364_shadowParams.z == 3.0f)
        {
            diagnostic = shadowVisibility.xxx;
        }
        if (_364_shadowParams.z == 4.0f)
        {
            diagnostic = albedo;
        }
        if (_364_shadowParams.z == 5.0f)
        {
            float param_20 = max(dot(N, V), 0.0f);
            float3 param_21 = F0;
            float param_22 = roughness;
            float3 dFil = FresnelSchlickRoughness(param_20, param_21, param_22);
            float3 dR = reflect(-V, N);
            float3 param_23 = dR;
            float param_24 = roughness;
            float3 dPre = SamplePrefilteredEnvironment(param_23, param_24);
            float2 dBrdf = brdfLUT.Sample(defaultSampler, float2(max(dot(N, V), 0.0f), roughness)).xy;
            float3 dSpecIBL = dPre * ((dFil * dBrdf.x) + dBrdf.y.xxx);
            diagnostic = ((dSpecIBL * 0.3499999940395355224609375f) * lerp(0.3499999940395355224609375f, 1.0f, shadowVisibility)) * ao;
        }
        outColor = float4(diagnostic, 1.0f);
        return;
    }
    bool _1053 = _364_shadowParams.x > 0.25f;
    bool _1059;
    if (_1053)
    {
        _1059 = _364_shadowParams.x < 0.75f;
    }
    else
    {
        _1059 = _1053;
    }
    if (_1059)
    {
        outColor = float4(shadowVisibility.xxx * 3.8461000919342041015625f, 1.0f);
        return;
    }
    bool _1073 = _364_shadowParams.x > 0.0500000007450580596923828125f;
    bool _1079;
    if (_1073)
    {
        _1079 = _364_shadowParams.x < 0.25f;
    }
    else
    {
        _1079 = _1073;
    }
    if (_1079)
    {
        outColor = float4(albedo, 1.0f);
        return;
    }
    float3 Lo = ((((diffuse + specular) * _364_lightColor.xyz) * NdotL) * shadowVisibility) * 50.0f;
    float trans = (translucentTex.Sample(defaultSampler, inUv).x * _364_shadowParams.w) * transmissionMask;
    Lo += (((albedo * _364_lightColor.xyz) * trans) * 50.0f);
    outColor = float4(Lo, 1.0f);
    if (_364_ambientLight.w < 0.5f)
    {
        return;
    }
    float param_25 = max(dot(N, V), 0.0f);
    float3 param_26 = F0;
    float param_27 = roughness;
    float3 Fil = FresnelSchlickRoughness(param_25, param_26, param_27);
    float3 kSil = Fil;
    float3 kDil = (1.0f.xxx - kSil) * (1.0f - metallic);
    float3 irradiance = irradianceMap.Sample(defaultSampler, N).xyz;
    float3 diffuseIBL = irradiance * albedo;
    float3 R = reflect(-V, N);
    float3 param_28 = R;
    float param_29 = roughness;
    float3 prefilteredColor = SamplePrefilteredEnvironment(param_28, param_29);
    float2 brdf = brdfLUT.Sample(defaultSampler, float2(max(dot(N, V), 0.0f), roughness)).xy;
    float3 specularIBL = prefilteredColor * ((Fil * brdf.x) + brdf.y.xxx);
    if (_364_ambientLight.w > 1.5f)
    {
        outEnvironmentSpecular = float4((specularIBL * 0.3499999940395355224609375f) * clamp(orm.x, 0.0f, 1.0f), 1.0f);
        return;
    }
    float ambientVisibility = lerp(0.0500000007450580596923828125f, 1.0f, shadowVisibility);
    float specularVisibility = lerp(0.3499999940395355224609375f, 1.0f, shadowVisibility);
    float3 ambient = ((((kDil * diffuseIBL) * 0.0f) * ambientVisibility) + ((specularIBL * 0.3499999940395355224609375f) * specularVisibility)) * ao;
    outColor = float4(Lo, 1.0f);
    outEnvironmentSpecular = float4(ambient, 1.0f);
}

SPIRV_Cross_Output main(SPIRV_Cross_Input stage_input)
{
    inUv = stage_input.inUv;
    frag_main();
    SPIRV_Cross_Output stage_output;
    stage_output.outEnvironmentSpecular = outEnvironmentSpecular;
    stage_output.outColor = outColor;
    return stage_output;
}
