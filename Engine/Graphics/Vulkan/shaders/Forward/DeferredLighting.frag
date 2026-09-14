#version 450 core

// T4.6.5 part 37 — Cook-Torrance PBR + IBL port of Metal
// `fragmentLighting_v3` (DeferredLighting.metal:197-290).
//
// Differences from Metal original:
//   - IBL: bindings 10/11/12 wired (irradiance cube + prefilter cube + brdfLUT
//     2D) via DeferredLightingModule SetIBLResources. When handles are
//     INVALID, C++ binds a 1x1 white fallback — sampling returns white, so
//     IBL term becomes white * albedo (slight bright tint). Production code
//     path uses real env map (TestVulkanSponzaRenderGraph loads sunset.hdr).
//   - No ACES tonemap: output HDR linear. Tonemap is the FinalBlit
//     module's responsibility (matches Metal fragmentBlit).
//   - Y convention: Vulkan NDC Y is unflipped at vertex stage; UV origin
//     is bottom-left (matches Blit.vert convention).
//   - Shadow: samples pre-filtered `shadowVisTex` directly (ShadowMapModule
//     output) rather than reprojecting via shadowMatrix0.
//
// Entry point: main.
//
// Descriptor layout (matches DeferredLightingModule::Initialize):
//   set 0 binding 0 = UniformBuffer ViewData (Vertex|Pixel)
//   set 0 binding 1 = UniformBuffer SceneData
//   set 0 binding 2 = SampledImage albedo
//   set 0 binding 3 = SampledImage normal
//   set 0 binding 4 = SampledImage orm
//   set 0 binding 5 = SampledImage depth
//   set 0 binding 6 = SampledImage shadowVisibility
//   set 0 binding 8 = Sampler
//   set 0 binding 9 = SampledImage fallback (unused)
//   set 0 binding 10 = SampledImage irradianceMap (cube, IBL)
//   set 0 binding 11 = SampledImage prefilterMap (cube, IBL)
//   set 0 binding 12 = SampledImage brdfLUT (2D, IBL)

#extension GL_EXT_samplerless_texture_functions : enable

const float PI = 3.14159265358979;

// T4.6.5 part 40.3: visual contrast tuning.
//   - DIRECT_INTENSITY: boosts direct light so lit regions read brighter.
//   - IBL_INTENSITY: scales overall IBL ambient (was 1.0 → too flat / wash).
//   - SHADOW_AMBIENT_SCALE: ambient is multiplied by
//     mix(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility), so shadowed pixels
//     get only SHADOW_AMBIENT_SCALE × IBL_INTENSITY → shadows read darker.
// T8.10h: 2.5→3.125 (+25%). User-audit: sunlit p95 ≈153/255 read as
// overcast; sunlit-region indirect share measured 32% (physical 15-25%).
// This knob scales SCREEN direct only — camera light_color also drives
// probe rays (×3.0 into the SC↔probe loop), so raising that instead would
// shift the loop equilibrium (T8.4 lesson).
// 2026-09-01 GI-观感恢复：按 8/14–8/28（直射校准前）的历史栈复原——
// DIRECT 2.5 × light_color 5.0 = 12.5 屏幕直射；IBL/阴影 ambient 维持
// part 40.3 压制值（该时代的阴影暖填充来自 ddgi_light_color=20 的探针
// 弹射，不是 skycolor 洗涤）。
// 2026-09-06 UE 参考对齐：跟随 Metal gpuDriven 校准值（EngineTest
// DeferredLighting.metal 54c9b8c2）——DIRECT_INTENSITY 2.5 → 5.0；
// IBL ambient 归零（Metal gpuDriven 同位置 ambient=0，环境光由 DDGI
// fusion 全权负责，避免第二环境光源把拱门阴影填亮）。
// 2026-09-10 C4 首旋钮：GI 修复（探针索引/步长封顶/C3.1）后室内拾亮，
// 直射相对冲击力下降（用户目测"直射偏弱"）。7.5 → 10.0（+33%）。
// 只缩放屏幕直射——不进探针回路，不移动 SC↔probe 平衡（T8.4 教训）。
const float DIRECT_INTENSITY   = 50.0;   // 2026-09-10 十七: 用户实验 20→50（SC 尺度同步 ×2.5，C2.1 单位统一保持）
const float IBL_INTENSITY      = 0.0;
const float SHADOW_AMBIENT_SCALE = 0.05;

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    mat4 invViewProjection;
};

// 2026-09-07 修复：对齐 C++ SceneData 416B 新布局（be0dcf12 字段重排——
// jitter/previousJitter 前移至 208/216，新增 time/viewPos，shadowMatrix
// 后移 240/304，尾部 ambientLight）。旧 384B 块使 shadowMatrix0/1 读到
// time/viewPos 字节 → Chebyshev 全域≈0 → 直射×0（Vulkan 无 Metal 的
// argument-length 校验，静默错位）。
layout(set = 0, binding = 1) uniform SceneData {
    mat4 model;
    vec4 lightPos;
    vec4 lightColor;
    vec4 reflectionPlane;
    vec4 reflectionPlane2;
    vec4 reflectionPlane3;
    mat4 previousModel;
    vec2 jitter;
    vec2 previousJitter;
    float time;
    float _timePad;
    vec4 viewPos;
    mat4 shadowMatrix0;
    mat4 shadowMatrix1;
    // x = shadow mode (0 = pre-filtered R8 visibility, 1 = VSM moments +
    // Chebyshev), y = cascade 0 split distance, zw unused.
    vec4 shadowParams;
    // rgb = scene ambient color, a = intensity.
    vec4 ambientLight;
};

layout(set = 0, binding = 2) uniform texture2D albedoTex;
layout(set = 0, binding = 3) uniform texture2D normalTex;
layout(set = 0, binding = 4) uniform texture2D ormTex;
layout(set = 0, binding = 5) uniform texture2D depthTex;
layout(set = 0, binding = 6) uniform texture2D shadowVisTex;
layout(set = 0, binding = 7) uniform texture2D ssaoTex;   // SSAO filter output (R16F, 1.0=unoccluded)
layout(set = 0, binding = 8) uniform sampler defaultSampler;
layout(set = 0, binding = 9) uniform texture2D fallbackTex;  // unused
// T4.6.5 part 37: IBL resources (cube + 2D).
layout(set = 0, binding = 10) uniform textureCube irradianceMap;
layout(set = 0, binding = 11) uniform textureCube prefilterMap;
layout(set = 0, binding = 12) uniform texture2D brdfLUT;
// VSM blurred RG32 shadow moments per cascade (Chebyshev upper bound).
layout(set = 0, binding = 13) uniform texture2D shadowMoments0;
layout(set = 0, binding = 14) uniform texture2D shadowMoments1;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

// Chebyshev upper bound for VSM — mirrors RHIShaderFunctions.glsl
// ChebyshevUpperBound (minVariance = VSM_MIN_VARIANCE = 2e-5, light-bleed
// reduction via smoothstep 0.05..1.0).
float chebyshevVisibility(vec2 moments, float t) {
    if (t <= moments.x) return 1.0;
    float variance = moments.y - (moments.x * moments.x);
    variance = max(variance, 0.00002);
    float d = t - moments.x;
    float pMax = variance / (variance + d * d);
    return smoothstep(0.05, 1.0, pMax);
}

// PBR helpers — translated from
// Engine/Graphics/Metal/shaders/CommonFunction.metal:256-293 +
// Engine/Graphics/RHI/Shaders/RHIShaderPBR.metal:124-150.

float DistributionGGX(vec3 N, vec3 H, float a) {
    float a2 = a * a * a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float k) {
    float r = k + 1.0;
    float a = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float k) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), k)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), k);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// T4.6.5 part 37: roughness-aware Fresnel for IBL (mirror Metal
// FresnelSchlickRoughness in RHIShaderPBR.metal). The roughness term blends
// F0 toward 1.0 as roughness increases — gives metals a softer grazing tint.
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
         * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    vec4 albedoV = texture(sampler2D(albedoTex, defaultSampler), inUv);
    vec3 albedo = albedoV.rgb;
    vec3 normalEnc = texture(sampler2D(normalTex, defaultSampler), inUv).rgb;
    vec3 orm = texture(sampler2D(ormTex, defaultSampler), inUv).rgb;
    float depth = texture(sampler2D(depthTex, defaultSampler), inUv).r;


    // 2026-09-06 P1 天空接入：背景像素写程序化天空（原输出黑底）。
    // B15 (2026-09-11)：常数 ×2.0 重标——旧值按 passthrough ×0.26 线性直出
    // 标定（可见开口 ~182-210/255），B10.5 换 ACES×0.10 后同一 HDR 值显示
    // 只有 ~123/255。目标回到 UE 参考 ~210/255： zenith 2.88×0.10=0.288 →
    // ACES≈0.41 → gamma ≈0.67（123）；×2.0 → 0.576 → ACES≈0.70 → ≈0.85
    // （216）。只影响可见天空像素的显示——探针 GI 供给走独立的
    // DDGI_SKY_COLOR×SkyColor.x 门（C3.3），此改动不进 GI 回路。
    if (depth >= 1.0) {
        if (shadowParams.z > 0.5) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        vec2 skyNdc = vec2(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0);
        vec4 pFar = invViewProjection * vec4(skyNdc, 1.0, 1.0);
        vec3 skyDir = normalize(pFar.xyz / max(pFar.w, 1e-6) - viewPos.xyz);
        float up = clamp(skyDir.y, 0.0, 1.0);
        vec3 horizonCol = vec3(4.40, 4.20, 3.80); // 暖白地平雾
        vec3 zenithCol  = vec3(5.50, 5.76, 6.00); // 亮淡蓝天顶（UE 参考实测 rgb 210,220,229）
        vec3 sky = mix(horizonCol, zenithCol, pow(up, 0.45));
        vec3 sunL = normalize(lightPos.xyz);
        float sunAmount = pow(clamp(dot(skyDir, sunL), 0.0, 1.0), 16.0);
        sky += vec3(1.0, 0.85, 0.6) * sunAmount * 1.2; // 前向散射日晕
        outColor = vec4(sky, 1.0);
        return;
    }

    // Reconstruct world position. Y is FLIPPED (1 - uv.y*2) to match naga's
    // auto-inserted Y-negate in the WGSL GBuffer raster — the same
    // convention ShadowFilter.comp uses (T4.6.5 part 40.2) and the Metal
    // original uses. The previous unflipped port mirrored world-space Y:
    // every shadow lookup sampled the mirrored location, inverting the
    // apparent light direction.
    vec2 ndcXY = vec2(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0);
    vec4 ndc = vec4(ndcXY, depth, 1.0);
    vec4 worldPosH = invViewProjection * ndc;
    vec3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // Decode GBuffer.
    vec3 N = normalize(normalEnc * 2.0 - 1.0);
    // SSAO: read filter output at full-res. If SSAO pass is disabled, texture
    // falls back to white (1.0 = unoccluded) — no darkening.
    vec4 ssao = texture(sampler2D(ssaoTex, defaultSampler), inUv);
    float ao = (ssao.r > 0.0) ? ssao.r : 1.0;
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // Light + view vectors.
    vec3 V = normalize(viewPos.xyz - worldPos);
    vec3 L = normalize(lightPos.xyz);  // directional light
    vec3 H = normalize(V + L);

    // F0 mix: dielectric 0.04 → metal albedo.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance specular BRDF.
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    vec3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.001);

    // Energy-conserving diffuse.
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

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
        vec4 clip0 = shadowMatrix0 * vec4(worldPos, 1.0);
        vec3 sc0 = clip0.xyz / clip0.w;
        vec2 uv0 = vec2(sc0.x * 0.5 + 0.5, 0.5 - sc0.y * 0.5);
        bool inCascade0 = uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0 &&
                          sc0.z >= 0.0 && sc0.z <= 1.0;
        if (inCascade0) {
            vec2 moments = texture(sampler2D(shadowMoments0, defaultSampler), uv0).xy;
            shadowVisibility = chebyshevVisibility(moments, sc0.z);
        } else {
            // Cascade 1
            vec4 clip1 = shadowMatrix1 * vec4(worldPos, 1.0);
            vec3 sc1 = clip1.xyz / clip1.w;
            vec2 uv1 = vec2(sc1.x * 0.5 + 0.5, 0.5 - sc1.y * 0.5);
            bool inCascade1 = uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0 &&
                              sc1.z >= 0.0 && sc1.z <= 1.0;
            if (inCascade1) {
                vec2 moments = texture(sampler2D(shadowMoments1, defaultSampler), uv1).xy;
                shadowVisibility = chebyshevVisibility(moments, sc1.z);
            }
            // Else: outside both shadow maps — lit.
        }
    } else {
        shadowVisibility = texture(sampler2D(shadowVisTex, defaultSampler), inUv).r;
    }
    // B16 (2026-09-11): 曾加 vis<0.03 噪声门——已回退。锁相机 noAll+gate A/B
    // 证明拱腹"室内暖光"是 PCSS 大面积泄漏（vis≈0.1-0.3，见 SurfaceCache/
    // ShadowFilter 排查），不是 <3% 的预滤波底噪，此门无效且经 prevColor
    // 供给回路引入不可控的均衡漂移。真正的修复在 PCSS 链（ShadowDepth bias /
    // ShadowFilter blocker 搜索），见后续 B17。

    // Raw diagnostics: no exposure, gamma, GI, or temporal filtering.
    if (shadowParams.z > 0.5) {
        vec3 diagnostic = normalEnc;
        if (shadowParams.z == 2.0) diagnostic = vec3(NdotL);
        if (shadowParams.z == 3.0) diagnostic = vec3(shadowVisibility);
        if (shadowParams.z == 4.0) diagnostic = albedo;
        outColor = vec4(diagnostic, 1.0);
        return;
    }

    // PRIMAL_SHADOW_VIS_DEBUG (mode 0.5): PCSS branch above ran normally —
    // direct-output its result so the ShadowFilter output is visible on
    // screen without RenderDoc. ×(1/0.26) pre-compensates the tonemap
    // passthrough CAL_EXPOSURE so grayscale reads at native visibility.
    if (shadowParams.x > 0.25 && shadowParams.x < 0.75) {
        outColor = vec4(vec3(shadowVisibility) * 3.8461, 1.0);
        return;
    }
    // mode 0.25 (PRIMAL_GBUFFER_ALBEDO_DEBUG): raw GBuffer albedo, no
    // lighting — verifies the texture sRGB decode path end to end.
    if (shadowParams.x > 0.05 && shadowParams.x < 0.25) {
        outColor = vec4(albedo, 1.0);
        return;
    }

    vec3 Lo = (diffuse + specular) * lightColor.rgb * NdotL * shadowVisibility
            * DIRECT_INTENSITY;

    // T4.6.5 part 37: IBL ambient (image-based lighting).
    // Mirror Metal fragmentLighting_v3 lines 270-287. Cube samples use the
    // shared defaultSampler (linear + repeat — fine for env maps). Prefilter
    // uses textureLod with roughness × MAX_PREFILTER_LOD to pick the mip.
    vec3 Fil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kSil = Fil;
    vec3 kDil = (vec3(1.0) - kSil) * (1.0 - metallic);

    vec3 irradiance = texture(samplerCube(irradianceMap, defaultSampler), N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    const float MAX_PREFILTER_LOD = 4.0;
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(samplerCube(prefilterMap, defaultSampler),
                                       R, roughness * MAX_PREFILTER_LOD).rgb;
    vec2 brdf = texture(sampler2D(brdfLUT, defaultSampler),
                        vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (Fil * brdf.x + brdf.y);

    // T4.6.5 part 40.3: scale overall IBL down, and further crush ambient in
    // shadowed pixels so the shadow side reads darker against lit regions.
    float ambientVisibility = mix(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility);
    vec3 ambient = (kDil * diffuseIBL + specularIBL) * ao
                 * IBL_INTENSITY * ambientVisibility;

    outColor = vec4(Lo + ambient, 1.0);
}
