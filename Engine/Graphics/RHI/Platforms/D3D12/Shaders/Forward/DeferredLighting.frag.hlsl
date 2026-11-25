// Vulkan reference: Forward/DeferredLighting.frag (main).
#include "../Common/TextureDimensions.hlsli"

cbuffer SceneData : register(b1, space0)
{
    row_major float4x4 model : packoffset(c0);
    float4 lightPos : packoffset(c4);
    float4 lightColor : packoffset(c5);
    float4 reflectionPlane : packoffset(c6);
    float4 reflectionPlane2 : packoffset(c7);
    float4 reflectionPlane3 : packoffset(c8);
    row_major float4x4 previousModel : packoffset(c9);
    float2 jitter : packoffset(c13);
    float2 previousJitter : packoffset(c13.z);
    float time : packoffset(c14);
    float timePad : packoffset(c14.y);
    float4 viewPos : packoffset(c15);
    row_major float4x4 shadowMatrix0 : packoffset(c16);
    row_major float4x4 shadowMatrix1 : packoffset(c20);
    row_major float4x4 shadowMatrix2 : packoffset(c24);
    row_major float4x4 shadowMatrix3 : packoffset(c28);
    float4 shadowParams : packoffset(c32);
    float4 ambientLight : packoffset(c33);
};

cbuffer ViewData : register(b0, space0)
{
    row_major float4x4 viewProjection : packoffset(c0);
    row_major float4x4 invViewProjection : packoffset(c4);
};

ByteAddressBuffer probeDepthSSBO : register(t18, space0);
cbuffer ProbeParamsUBO : register(b19, space0)
{
    float4 probeOrigin_spacing : packoffset(c0);
    float4 probeCounts : packoffset(c1);
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


struct StageInput
{
    float2 inUv : TEXCOORD0;
    float4 fragCoord : SV_Position;
};

struct StageOutput
{
    float4 outColor : SV_Target0;
    float4 outEnvironmentSpecular : SV_Target1;
};

float3 EvaluateSkyBase(float3 dir) {
    float up = clamp(dir.y, 0.0, 1.0);
    float3 horizonCol = float3(4.40, 4.20, 3.80); // 暖白地平雾
    float3 zenithCol  = float3(5.50, 5.76, 6.00); // 亮淡蓝天顶
    return lerp(horizonCol, zenithCol, pow(up, 0.45));
}

float3 EvaluateSky(float3 dir, float3 sunDir, float3 sunLinearColor)
{
    float3 param = dir;
    float3 sky = EvaluateSkyBase(param);
    float sunAmount = pow(clamp(dot(dir, sunDir), 0.0f, 1.0f), 16.0f);
    float lum = dot(sunLinearColor, float3(0.2126f, 0.7152f, 0.0722f));
    float3 _111;
    if (lum > 0.0001f)
    {
        _111 = sunLinearColor * (0.86384f / lum);
    }
    else
    {
        _111 = float3(1.0f, 0.85f, 0.6f);
    }
    float3 halo = _111;
    sky += ((halo * sunAmount) * 1.2f);
    return sky;
}

float DistributionGGX(float3 N, float3 H, float a)
{
    float a2 = ((a * a) * a) * a;
    float NdotH = max(dot(N, H), 0.0f);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0f)) + 1.0f;
    denom = (3.1415927f * denom) * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float k) {
    float r = k + 1.0;
    float a = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float k)
{
    float param = max(dot(N, V), 0.0f);
    float param_1 = k;
    float param_2 = max(dot(N, L), 0.0f);
    float param_3 = k;
    return GeometrySchlickGGX(param, param_1) * GeometrySchlickGGX(param_2, param_3);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float chebyshevVisibility(float2 moments, float t) {
    if (t <= moments.x) return 1.0;
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, 0.00002);
    float d = t - moments.x;
    float pMax = variance / (variance + d * d);
    return smoothstep(0.05, 1.0, pMax);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    return F0 + (max((float3)(1.0 - roughness), F0) - F0)
         * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float3 SamplePrefilteredEnvironment(float3 direction, float roughness)
{
    int _287;
    uint cubeWidth, cubeHeight;
    prefilterMap.GetDimensions(0u, cubeWidth, cubeHeight, _287);
    float maxLod = float(max((int(_287) - 1), 0));
    return prefilterMap.SampleLevel(defaultSampler, direction, clamp(roughness, 0.0f, 1.0f) * maxLod).xyz;
}

static const float PI = 3.14159265358979;
static const float DIRECT_INTENSITY   = 50.0;
static const float IBL_DIFFUSE_INTENSITY  = 0.0;
static const float IBL_SPECULAR_INTENSITY = 0.35;
static const float SHADOW_AMBIENT_SCALE = 0.05;
static const float SHADOW_SPECULAR_SCALE = 0.35;

float2 probeOctDepth(float3 dir, uint pi) {
    float l1 = abs(dir.x) + abs(dir.y) + abs(dir.z);
    float2 uv = dir.xy / max(l1, 1e-6);
    if (dir.z < 0.0) {
        uv = (1.0 - abs(uv.yx)) * lerp((float2)(-1.0), (float2)(1.0),
             ((uv.xy) >= ((float2)(0.0))));
    }
    uv = uv * 0.5 + 0.5;
    int2 texel = int2(clamp(int(uv.x * 8.0), 0, 7), clamp(int(uv.y * 8.0), 0, 7));
    int idx = texel.y * 8 + texel.x;
    uint base = pi * 128u;   // DEPTH_FLOATS_PER_PROBE = 128（64 mean + 64 var）
    return float2(probeDepthSSBO.Load<float>((base + idx) * 4u), probeDepthSSBO.Load<float>((base + 64u + idx) * 4u));
}

// 最近探针 Chebyshev 可见性（DDGIGIGather.visibilityWeight 同式）。
// 返回 [0.2,1]；不可用返回 -1（调用方回退 stand-in）。
float probeSpecVisibility(float3 P) {
    if (probeCounts.w < 0.5) return -1.0;
    float3 gridf = (P - probeOrigin_spacing.xyz) / max(probeOrigin_spacing.w, 1e-4);
    float3 c = clamp(floor(gridf + 0.5), (float3)(0.0), probeCounts.xyz - (float3)(1.0));
    uint pi = uint(c.x + c.y * probeCounts.x + c.z * probeCounts.x * probeCounts.y);
    float3 probePos = probeOrigin_spacing.xyz + c * probeOrigin_spacing.w;
    float3 toProbe = probePos - P;
    float dist = length(toProbe);
    float2 mv = probeOctDepth(-toProbe, pi);  // 探针→点方向（gather 同口径）
    if (mv.y < 0.0001) return -1.0;
    float d = max(dist - mv.x, 0.0);
    float cheb = mv.y / (mv.y + d * d);
    return clamp(max(cheb * 2.0, 0.2), 0.0, 1.0);
}

StageOutput main(StageInput input) {
    StageOutput output;
    output.outEnvironmentSpecular = (float4)(0.0);
#ifdef PRIMAL_D3D12_DEFERRED_DIAGNOSTICS
    // Only the scene constant buffer is required for the draw diagnostics.
    if (shadowParams.z == 8.0 || shadowParams.z == 9.0) {
        output.outColor = (shadowParams.z == 8.0) ? float4((float3)(0.5), 1.0)
            : float4(input.fragCoord.xy / float2(2560.0, 1440.0), 0.0, 1.0);
        return output;
    }
#endif
    float4 albedoV = albedoTex.Sample(defaultSampler, input.inUv);
    float3 albedo = albedoV.rgb;
    float3 normalEnc = normalTex.Sample(defaultSampler, input.inUv).rgb;
    float3 orm = ormTex.Sample(defaultSampler, input.inUv).rgb;
    // P2-B: ORM.a 现在携带 per-instance 透光材质掩码（GBuffer 写入：
    // 石材/水体/unlit=0，布料实例>0）。旧 GBuffer 全家族写常数 1.0，
    // 透光项无法区分材质——石墙背光面也会被加"透光"。
    float transmissionMask = ormTex.Sample(defaultSampler, input.inUv).a;
    float depth = depthTex.Sample(defaultSampler, input.inUv).r;


    // 2026-09-06 P1 天空接入：背景像素写程序化天空（原输出黑底）。
    // B15 (2026-09-11)：常数 ×2.0 重标——旧值按 passthrough ×0.26 线性直出
    // 标定（可见开口 ~182-210/255），B10.5 换 ACES×0.10 后同一 HDR 值显示
    // 只有 ~123/255。目标回到 UE 参考 ~210/255： zenith 2.88×0.10=0.288 →
    // ACES≈0.41 → gamma ≈0.67（123）；×2.0 → 0.576 → ACES≈0.70 → ≈0.85
    // （216）。只影响可见天空像素的显示——探针 GI 供给走独立的
    // DDGI_SKY_COLOR×SkyColor.x 门（C3.3），此改动不进 GI 回路。
    if (depth >= 1.0) {
        if (shadowParams.z > 0.5 && shadowParams.z < 10.5) {
            output.outColor = float4(0.0, 0.0, 0.0, 1.0);
            return output;
        }
        float2 skyNdc = float2(input.inUv.x * 2.0 - 1.0, 1.0 - input.inUv.y * 2.0);
        float4 pFar = mul(float4(skyNdc, 1.0, 1.0), invViewProjection);
        float3 skyDir = normalize(pFar.xyz / max(pFar.w, 1e-6) - viewPos.xyz);
        // P1.1: 内联程序化天空 → 共享 EvaluateSky（含前向散射日晕），
        // 数值与原实现一致；GI miss 项经同文件 EvaluateSkyBase 统一档位。
        // sun-audit §7 P2: 日晕色度跟随太阳线性色（Y 归一保能量档）。
        float3 sky = EvaluateSky(skyDir, normalize(lightPos.xyz), lightColor.rgb);
        output.outColor = float4(sky, 1.0);
        return output;
    }

    // Reconstruct world position. Y is FLIPPED (1 - uv.y*2) to match naga's
    // auto-inserted Y-negate in the WGSL GBuffer raster — the same
    // convention ShadowFilter.comp uses (T4.6.5 part 40.2) and the Metal
    // original uses. The previous unflipped port mirrored world-space Y:
    // every shadow lookup sampled the mirrored location, inverting the
    // apparent light direction.
    float2 ndcXY = float2(input.inUv.x * 2.0 - 1.0, 1.0 - input.inUv.y * 2.0);
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 worldPosH = mul(ndc, invViewProjection);
    float3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // Decode GBuffer.
    float3 N = normalize(normalEnc * 2.0 - 1.0);
    // SSAO: read filter output at full-res. If SSAO pass is disabled, texture
    // falls back to white (1.0 = unoccluded) — no darkening.
    float4 ssao = ssaoTex.Sample(defaultSampler, input.inUv);
    float ao = (ssao.r > 0.0) ? ssao.r : 1.0;
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // sun-audit P2 材质消融（PRIMAL_MATERIAL_ABLATION → shadowParams.z+10
    // 编码，与 PRIMAL_LIGHTING_DEBUG 互斥；layout 零变更）：
    //   11 = gray_albedo（反照率 → 0.5 灰，定位强饱和来源）
    //   12 = flat_normal（法线 → 常量上向量，定位法线驱动的高频明暗）
    //   13 = fixed_roughness(=1)（完全粗糙，定位镜面太阳辉点）
    // 只消融直射光照响应的材质输入；GI 供给（SC 缓存）保持真实材质，
    // 避免同时改掉 GI 弹射源。
    if (shadowParams.z >= 10.5) {
        if (shadowParams.z < 11.5) albedo = (float3)(0.5);
        else if (shadowParams.z < 12.5) N = float3(0.0, 1.0, 0.0);
        else if (shadowParams.z < 13.5) roughness = 1.0;
    }

    // Light + view vectors.
    float3 V = normalize(viewPos.xyz - worldPos);
    float3 L = normalize(lightPos.xyz);  // directional light
    float3 H = normalize(V + L);

    // F0 mix: dielectric 0.04 → metal albedo.
    float3 F0 = lerp((float3)(0.04), albedo, metallic);

    // Cook-Torrance specular BRDF.
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.001);

    // Energy-conserving diffuse.
    float3 kS = F;
    float3 kD = ((float3)(1.0) - kS) * (1.0 - metallic);
    float3 diffuse = kD * albedo / PI;

    // Shadow visibility.
    //   mode 0 (PCSS): ShadowMapModule's pre-filtered R8 visibility texture.
    //   mode 0.5 (PRIMAL_SHADOW_VIS_DEBUG): PCSS vis sampled normally, then
    //          output as grayscale — visualizes the ShadowFilter output
    //          (×(1/0.26) pre-compensates the tonemap passthrough exposure).
    //   mode 1 (VSM): project worldPos into the cascade's light clip space,
    //                 sample blurred (z, z²) moments, Chebyshev upper bound.
    // Cascade selection is BOUNDS-based with fallback (cascade 0 → 1 → lit),
    // mirroring ShadowFilter.comp — a distance split leaves a hard boundary
    // where the cascade-0 ortho box (30 world units) ends.
    // The Y-flip (0.5 - y*0.5) matches naga's auto-inserted OpFNegate on
    // gl_Position.y in ShadowDepth.wgsl (T4.6.5 part 40.1 — the same fix the
    // PCSS path needed; regressing it mirrors the shadow map vertically).
    float shadowVisibility;
    if (shadowParams.x > 0.75) {
        shadowVisibility = 1.0;
        // Cascade 0
        float4 clip0 = mul(float4(worldPos, 1.0), shadowMatrix0);
        float3 sc0 = clip0.xyz / clip0.w;
        float2 uv0 = float2(sc0.x * 0.5 + 0.5, 0.5 - sc0.y * 0.5);
        bool inCascade0 = uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0 &&
                          sc0.z >= 0.0 && sc0.z <= 1.0;
        if (inCascade0) {
            float2 moments = shadowMoments0.Sample(defaultSampler, uv0).xy;
            shadowVisibility = chebyshevVisibility(moments, sc0.z);
        } else {
            // Cascade 1
            float4 clip1 = mul(float4(worldPos, 1.0), shadowMatrix1);
            float3 sc1 = clip1.xyz / clip1.w;
            float2 uv1 = float2(sc1.x * 0.5 + 0.5, 0.5 - sc1.y * 0.5);
            bool inCascade1 = uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0 &&
                              sc1.z >= 0.0 && sc1.z <= 1.0;
            if (inCascade1) {
                float2 moments = shadowMoments1.Sample(defaultSampler, uv1).xy;
                shadowVisibility = chebyshevVisibility(moments, sc1.z);
            } else {
                // P4.2: 级联 2/3（由近到远）
                float4 clip2 = mul(float4(worldPos, 1.0), shadowMatrix2);
                float3 sc2 = clip2.xyz / clip2.w;
                float2 uv2 = float2(sc2.x * 0.5 + 0.5, 0.5 - sc2.y * 0.5);
                bool inCascade2 = uv2.x >= 0.0 && uv2.x <= 1.0 && uv2.y >= 0.0 && uv2.y <= 1.0 &&
                                  sc2.z >= 0.0 && sc2.z <= 1.0;
                if (inCascade2) {
                    float2 moments = shadowMoments2.Sample(defaultSampler, uv2).xy;
                    shadowVisibility = chebyshevVisibility(moments, sc2.z);
                } else {
                    float4 clip3 = mul(float4(worldPos, 1.0), shadowMatrix3);
                    float3 sc3 = clip3.xyz / clip3.w;
                    float2 uv3 = float2(sc3.x * 0.5 + 0.5, 0.5 - sc3.y * 0.5);
                    if (uv3.x >= 0.0 && uv3.x <= 1.0 && uv3.y >= 0.0 && uv3.y <= 1.0 &&
                        sc3.z >= 0.0 && sc3.z <= 1.0) {
                        float2 moments = shadowMoments3.Sample(defaultSampler, uv3).xy;
                        shadowVisibility = chebyshevVisibility(moments, sc3.z);
                    }
                    // 全部越界：lit
                }
            }
        }
    } else {
        shadowVisibility = shadowVisTex.Sample(defaultSampler, input.inUv).r;
    }
    // B16 (2026-09-11): 曾加 vis<0.03 噪声门——已回退。锁相机 noAll+gate A/B
    // 证明拱腹"室内暖光"是 PCSS 大面积泄漏（vis≈0.1-0.3，见 SurfaceCache/
    // ShadowFilter 排查），不是 <3% 的预滤波底噪，此门无效且经 prevColor
    // 供给回路引入不可控的均衡漂移。真正的修复在 PCSS 链（ShadowDepth bias /
    // ShadowFilter blocker 搜索），见后续 B17。

    // Raw diagnostics: no exposure, gamma, GI, or temporal filtering.
    // （z+10 = 材质消融档，不是诊断视图——消融走完整光照，此处必须放行）
    if (shadowParams.z > 0.5 && shadowParams.z < 10.5) {
        float3 diagnostic = normalEnc;
        if (shadowParams.z == 2.0) diagnostic = (float3)(NdotL);
        if (shadowParams.z == 3.0) diagnostic = (float3)(shadowVisibility);
        if (shadowParams.z == 4.0) diagnostic = albedo;
        // R49（2026-11-22）：Load 对照模式——D3D12 上 z=4（Sample albedo）与
        // z=3（Sample vis）双双全零而纹理内容活（dump 面）、签名自洽、
        // ShadowFilter 的 SampleLevel 活。z=6/7 用 texelFetch（无采样器，
        // 与探针同机制）采样同一批纹理：z=6 活而 z=4 零 → 采样器侧死
        // （deferred set 的采样器表物化/绑定）；z=6 亦零 → SRV 描述符投递
        // 死（set 区段槽位）。canonical（z=0）行为零变化。
        if (shadowParams.z == 6.0) diagnostic = albedoTex.Load(int3((int2)(input.fragCoord.xy), 0)).rgb;
        if (shadowParams.z == 7.0) {
            int2 visSize = textureDimensions(shadowVisTex, 0);
            int2 scrSize = textureDimensions(albedoTex, 0);
            int2 vp2 = (int2)((float2)(input.fragCoord.xy) * ((float2)(visSize) / max((float2)(scrSize), (float2)(1.0))));
            diagnostic = (float3)(shadowVisTex.Load(int3(clamp(vp2, (int2)(0), visSize - (int2)(1)), 0)).r);
        }
#ifndef PRIMAL_D3D12_DEFERRED_DIAGNOSTICS
        if (shadowParams.z == 8.0) diagnostic = (float3)(0.5);
        if (shadowParams.z == 9.0) diagnostic = float3(input.fragCoord.xy / float2(2560.0, 1440.0), 0.0);
#endif
        if (shadowParams.z == 5.0) {
            // sun-audit P2: IBL ambient 项直出（环境镜面解耦 A/B 测量）。
            // IBL_DIFFUSE_INTENSITY=0 时 = 纯环境镜面；可见性处理与主路径
            // 同分支（stand-in 地板或 PRIMAL_SPEC_OCC 的 GTAO 遮挡）。
            float3 dFil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
            float3 dR = reflect(-V, N);
            float3 dPre = SamplePrefilteredEnvironment(dR, roughness);
            float2 dBrdf = brdfLUT.Sample(defaultSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
            float3 dSpecIBL = dPre * (dFil * dBrdf.x + dBrdf.y);
#if defined(PRIMAL_PROBE_OCC)
            float dPv = probeSpecVisibility(worldPos);
            float dVis2 = dPv < 0.0 ? lerp(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) : dPv;
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY * dVis2;
#elif defined(PRIMAL_SPEC_OCC)
            const float dOcc = clamp(pow(max(dot(N, V), 0.0) + ao, 4.0) - 1.0 + ao, 0.0, 1.0);
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY * dOcc;
#else
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY
                       * lerp(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) * ao;
#endif
        }
        output.outColor = float4(diagnostic, 1.0);
        return output;
    }

    // PRIMAL_SHADOW_VIS_DEBUG (mode 0.5): PCSS branch above ran normally —
    // direct-output its result so the ShadowFilter output is visible on
    // screen without RenderDoc. ×(1/0.26) pre-compensates the tonemap
    // passthrough CAL_EXPOSURE so grayscale reads at native visibility.
    if (shadowParams.x > 0.25 && shadowParams.x < 0.75) {
        output.outColor = float4((float3)(shadowVisibility) * 3.8461, 1.0);
        return output;
    }
    // mode 0.25 (PRIMAL_GBUFFER_ALBEDO_DEBUG): raw GBuffer albedo, no
    // lighting — verifies the texture sRGB decode path end to end.
    if (shadowParams.x > 0.05 && shadowParams.x < 0.25) {
        output.outColor = float4(albedo, 1.0);
        return output;
    }

    float3 Lo = (diffuse + specular) * lightColor.rgb * NdotL * shadowVisibility
            * DIRECT_INTENSITY;

    // P4.1: 布帘透光——阴影区背光透射合成。ShadowFilter 输出的透光强度
    // 已含厚度衰减与背面因子；shadowParams.w = 全局透光系数
    // （PRIMAL_TONE_TRANSLUCENCY，默认 0 = 关闭，画面零变化）。
    // P2-B: 再乘 ORM.a 材质掩码——只有标注为透光的薄布料实例生效，
    // 石墙系数严格为 0（不再依赖全局开关单独兜底材质区分）。
    {
        float trans = translucentTex.Sample(defaultSampler, input.inUv).r
                    * shadowParams.w * transmissionMask;
        Lo += albedo * lightColor.rgb * trans * DIRECT_INTENSITY;
    }

    output.outColor = float4(Lo, 1.0);
    if (ambientLight.a < 0.5) return output;

    // T4.6.5 part 37: IBL ambient (image-based lighting).
    // Mirror Metal fragmentLighting_v3 lines 270-287. Cube samples use the
    // shared defaultSampler (linear + repeat — fine for env maps). Prefilter
    // uses textureLod with roughness across the actual bound mip range.
    float3 Fil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kSil = Fil;
    float3 kDil = ((float3)(1.0) - kSil) * (1.0 - metallic);

    float3 irradiance = irradianceMap.Sample(defaultSampler, N).rgb;
    float3 diffuseIBL = irradiance * albedo;

    float3 R = reflect(-V, N);
    float3 prefilteredColor = SamplePrefilteredEnvironment(R, roughness);
    float2 brdf = brdfLUT.Sample(defaultSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specularIBL = prefilteredColor * (Fil * brdf.x + brdf.y);

    if (ambientLight.a > 1.5) {
        // The environment BRDF is separated from geometric visibility. The
        // latter is measured along the reflection lobe before post composite.
        // Keep only material micro-occlusion here: no sun-shadow/SSAO proxy.
        output.outEnvironmentSpecular = float4(specularIBL * IBL_SPECULAR_INTENSITY * clamp(orm.r,0.0,1.0), 1.0);
        return output;
    }

    // T4.6.5 part 40.3: scale overall IBL down, and further crush ambient in
    // shadowed pixels so the shadow side reads darker against lit regions.
    // P2-A: per-branch gating — diffuse off (DDGI owns ambient), specular on.
    // P2（reaudit §4.6）: 镜面/漫反射分支使用各自的阴影地板——太阳阴影不再
    // 直接决定环境镜面可见性。
    float ambientVisibility = lerp(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility);
    float specularVisibility = lerp(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility);
#if defined(PRIMAL_PROBE_OCC)
    // sun-audit P2 第二步（PRIMAL_SPEC_OCC=2 → PROBE_OCC 编译变体）：以
    // DDGI 探针 Chebyshev 可见性（真实方向几何遮挡）替换 GTAO 饱和近似
    // ——消除高 N·V+ao 处 specOcc≡1 的局限。探针不可用时回退 stand-in。
    float pv = probeSpecVisibility(worldPos);
    specularVisibility = pv < 0.0 ? lerp(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) : pv;
    float3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility) * ao
                 + specularIBL * IBL_SPECULAR_INTENSITY * specularVisibility;
#elif defined(PRIMAL_SPEC_OCC)
    // sun-audit P2 环境镜面解耦第一步（PRIMAL_SPEC_OCC 变体，编译期 -D）：
    // 以屏幕空间可见性（GTAO AO）+ N·V 的 GTAO 镜面遮挡近似**替换**太阳
    // shadowVisibility 代理——固定天空旋转太阳时环境镜面不再仅因太阳入
    // 阴影而骤降（审计验收线）。直射镜面（Cook-Torrance specular）仍用
    // shadowVisibility（直射本应被太阳阴影遮蔽）。遮挡公式自带 ao 项，
    // 故本分支不再外乘 ao。
    const float specOcc = clamp(pow(max(dot(N, V), 0.0) + ao, 4.0) - 1.0 + ao, 0.0, 1.0);
    float3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility) * ao
                 + specularIBL * IBL_SPECULAR_INTENSITY * specOcc;
#else
    float3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility
                  + specularIBL * IBL_SPECULAR_INTENSITY * specularVisibility)
                 * ao;
#endif

    output.outColor = float4(Lo, 1.0);
    output.outEnvironmentSpecular = float4(ambient, 1.0);
    return output;
}
