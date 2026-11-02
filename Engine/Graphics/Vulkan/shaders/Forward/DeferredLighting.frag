#version 450 core
#extension GL_GOOGLE_include_directive : require

// P1.1: 共享程序化天空（可见天空背景与 GI miss 项同源，路线图 P1.2）
#include "../Lumen/ShaderLib/SkyAtmosphere.glsl"

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
// P2-A（sponza-gap-audit §4.8）: diffuse 与 specular IBL 分开门控。
// 旧 IBL_INTENSITY=0 同时压掉两路——为防 diffuse 环境光与 DDGI fusion 重复
// 累加而把镜面反射也一起归零，石材只剩漫反射响应。现在 diffuse 保持 0
// （DDGI 全权负责 diffuse ambient，2026-09-06 校准不变），specular 以低系
// 数开启：0.35 对照 HDR 天空 4.2-6.0 量级给出宽而弱的环境高光，能量可解释。
// 注意此处 IBL cube 是独立加载的 HDR 环境，与解析天空未做单位统一；后续
// 校准只动 IBL_SPECULAR_INTENSITY，不回退 diffuse 门。
const float IBL_DIFFUSE_INTENSITY  = 0.0;
// 2026-10-23（用户定调）：环境镜面 IBL 归零——lightmap 互斥态下它曾是
// E≈0 暗区的唯一光（§20"暗部 fill"，B/R≈0.97-1.58 的残蓝来源，§21
// DARKTINT 即为补偿它）。归零后无直射+无间接的暗区=物理真黑。
// [2026-11-02 合并回收轮] 恢复 dev 的 0.35：§24 归零是 lightmap 分支的
// 观感决策，但按"合回 dev 不影响原有功能"的合并条件，默认路径源码须与
// dev 逐位一致。lightmap 态的暗部环境光由 dev 新增的 fusion
// environment-specular 通道提供（2026-11-02 互斥收窄后 lightmap 态放行，
// 物理来源优于旧 fill），DARKTINT 仍在 composite 端兜底。§24 的最终
// 去留转 lightmap 态专项重议。
const float IBL_SPECULAR_INTENSITY = 0.35;
const float SHADOW_AMBIENT_SCALE = 0.05;
// P2（sponza-gap-reaudit §4.6）: 环境镜面遮挡与日光阴影分离。旧式把太阳
// shadowVisibility 直接乘进 IBL specular（mix(0.05,1,vis)），阴影内环境镜面
// 被同步压掉 95%——太阳转到阴影侧时本应来自天空/建筑的反射一同消失。
// 镜面分支改用独立地板 0.35（阴影内天空仍经由开口可见，拱腹掠射 sheen
// 不再被日光阴影抹掉）；这是缺乏方向性环境可见性项之前的保守 stand-in，
// 漫反射分支维持 0.05 压制语义不变（当前 diffuse IBL=0，实际无操作）。
const float SHADOW_SPECULAR_SCALE = 0.35;

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
    mat4 shadowMatrix2;  // P4.2: 级联 2
    mat4 shadowMatrix3;  // P4.2: 级联 3
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

// sun-audit P2 第二步: DDGI 探针深度可见性（PRIMAL_PROBE_OCC 变体专用；
// 其余变体不访问。绑定常驻有效——探针不可用时 params.counts=0 早退，
// SSBO 绑 dummy 1-float 缓冲）。
layout(std430, set = 0, binding = 18) readonly buffer ProbeDepthSSBO {
    float ddgiProbeDepthBuffer[];
};
layout(set = 0, binding = 19) uniform ProbeParamsUBO {
    vec4 probeOrigin_spacing;  // xyz = grid origin, w = spacing (wu)
    vec4 probeCounts;          // xyz = counts, w = total (0 = 不可用)
};

// 与 DDGIGIGather 的 gatherOctEncode 同式：8×8 octahedral mean/variance。
vec2 probeOctDepth(vec3 dir, uint pi) {
    float l1 = abs(dir.x) + abs(dir.y) + abs(dir.z);
    vec2 uv = dir.xy / max(l1, 1e-6);
    if (dir.z < 0.0) {
        uv = (1.0 - abs(uv.yx)) * mix(vec2(-1.0), vec2(1.0),
             greaterThanEqual(uv.xy, vec2(0.0)));
    }
    uv = uv * 0.5 + 0.5;
    ivec2 texel = ivec2(clamp(int(uv.x * 8.0), 0, 7), clamp(int(uv.y * 8.0), 0, 7));
    int idx = texel.y * 8 + texel.x;
    uint base = pi * 128u;   // DEPTH_FLOATS_PER_PROBE = 128（64 mean + 64 var）
    return vec2(ddgiProbeDepthBuffer[base + idx],
                ddgiProbeDepthBuffer[base + 64u + idx]);
}

// 最近探针 Chebyshev 可见性（DDGIGIGather.visibilityWeight 同式）。
// 返回 [0.2,1]；不可用返回 -1（调用方回退 stand-in）。
float probeSpecVisibility(vec3 P) {
    if (probeCounts.w < 0.5) return -1.0;
    vec3 gridf = (P - probeOrigin_spacing.xyz) / max(probeOrigin_spacing.w, 1e-4);
    vec3 c = clamp(floor(gridf + 0.5), vec3(0.0), probeCounts.xyz - vec3(1.0));
    uint pi = uint(c.x + c.y * probeCounts.x + c.z * probeCounts.x * probeCounts.y);
    vec3 probePos = probeOrigin_spacing.xyz + c * probeOrigin_spacing.w;
    vec3 toProbe = probePos - P;
    float dist = length(toProbe);
    vec2 mv = probeOctDepth(-toProbe, pi);  // 探针→点方向（gather 同口径）
    if (mv.y < 0.0001) return -1.0;
    float d = max(dist - mv.x, 0.0);
    float cheb = mv.y / (mv.y + d * d);
    return clamp(max(cheb * 2.0, 0.2), 0.0, 1.0);
}
// VSM blurred RG32 shadow moments per cascade (Chebyshev upper bound).
layout(set = 0, binding = 13) uniform texture2D shadowMoments0;
layout(set = 0, binding = 14) uniform texture2D shadowMoments1;
layout(set = 0, binding = 15) uniform texture2D shadowMoments2;  // P4.2
layout(set = 0, binding = 16) uniform texture2D shadowMoments3;  // P4.2
layout(set = 0, binding = 17) uniform texture2D translucentTex;  // P4.1 透光强度

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
// Separate display reflection from the direct radiance sampled by GI.
layout(location = 1) out vec4 outEnvironmentSpecular;

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

vec3 SamplePrefilteredEnvironment(vec3 direction, float roughness) {
    // The prefilter bake distributes roughness over every mip in the bound
    // view. Query that view rather than assuming a five-level allocation.
    float maxLod = float(max(textureQueryLevels(samplerCube(prefilterMap, defaultSampler)) - 1, 0));
    return textureLod(samplerCube(prefilterMap, defaultSampler), direction,
                      clamp(roughness, 0.0, 1.0) * maxLod).rgb;
}

void main() {
    outEnvironmentSpecular = vec4(0.0);
    vec4 albedoV = texture(sampler2D(albedoTex, defaultSampler), inUv);
    vec3 albedo = albedoV.rgb;
    vec3 normalEnc = texture(sampler2D(normalTex, defaultSampler), inUv).rgb;
    vec3 orm = texture(sampler2D(ormTex, defaultSampler), inUv).rgb;
    // P2-B: ORM.a 现在携带 per-instance 透光材质掩码（GBuffer 写入：
    // 石材/水体/unlit=0，布料实例>0）。旧 GBuffer 全家族写常数 1.0，
    // 透光项无法区分材质——石墙背光面也会被加"透光"。
    float transmissionMask = texture(sampler2D(ormTex, defaultSampler), inUv).a;
    float depth = texture(sampler2D(depthTex, defaultSampler), inUv).r;


    // 2026-09-06 P1 天空接入：背景像素写程序化天空（原输出黑底）。
    // B15 (2026-09-11)：常数 ×2.0 重标——旧值按 passthrough ×0.26 线性直出
    // 标定（可见开口 ~182-210/255），B10.5 换 ACES×0.10 后同一 HDR 值显示
    // 只有 ~123/255。目标回到 UE 参考 ~210/255： zenith 2.88×0.10=0.288 →
    // ACES≈0.41 → gamma ≈0.67（123）；×2.0 → 0.576 → ACES≈0.70 → ≈0.85
    // （216）。只影响可见天空像素的显示——探针 GI 供给走独立的
    // DDGI_SKY_COLOR×SkyColor.x 门（C3.3），此改动不进 GI 回路。
    if (depth >= 1.0) {
        if (shadowParams.z > 0.5 && shadowParams.z < 10.5) {
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        vec2 skyNdc = vec2(inUv.x * 2.0 - 1.0, 1.0 - inUv.y * 2.0);
        vec4 pFar = invViewProjection * vec4(skyNdc, 1.0, 1.0);
        vec3 skyDir = normalize(pFar.xyz / max(pFar.w, 1e-6) - viewPos.xyz);
        // P1.1: 内联程序化天空 → 共享 EvaluateSky（含前向散射日晕），
        // 数值与原实现一致；GI miss 项经同文件 EvaluateSkyBase 统一档位。
        // sun-audit §7 P2: 日晕色度跟随太阳线性色（Y 归一保能量档）。
        vec3 sky = EvaluateSky(skyDir, normalize(lightPos.xyz), lightColor.rgb);
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

    // sun-audit P2 材质消融（PRIMAL_MATERIAL_ABLATION → shadowParams.z+10
    // 编码，与 PRIMAL_LIGHTING_DEBUG 互斥；layout 零变更）：
    //   11 = gray_albedo（反照率 → 0.5 灰，定位强饱和来源）
    //   12 = flat_normal（法线 → 常量上向量，定位法线驱动的高频明暗）
    //   13 = fixed_roughness(=1)（完全粗糙，定位镜面太阳辉点）
    // 只消融直射光照响应的材质输入；GI 供给（SC 缓存）保持真实材质，
    // 避免同时改掉 GI 弹射源。
    if (shadowParams.z >= 10.5) {
        if (shadowParams.z < 11.5) albedo = vec3(0.5);
        else if (shadowParams.z < 12.5) N = vec3(0.0, 1.0, 0.0);
        else if (shadowParams.z < 13.5) roughness = 1.0;
    }

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
            } else {
                // P4.2: 级联 2/3（由近到远）
                vec4 clip2 = shadowMatrix2 * vec4(worldPos, 1.0);
                vec3 sc2 = clip2.xyz / clip2.w;
                vec2 uv2 = vec2(sc2.x * 0.5 + 0.5, 0.5 - sc2.y * 0.5);
                bool inCascade2 = uv2.x >= 0.0 && uv2.x <= 1.0 && uv2.y >= 0.0 && uv2.y <= 1.0 &&
                                  sc2.z >= 0.0 && sc2.z <= 1.0;
                if (inCascade2) {
                    vec2 moments = texture(sampler2D(shadowMoments2, defaultSampler), uv2).xy;
                    shadowVisibility = chebyshevVisibility(moments, sc2.z);
                } else {
                    vec4 clip3 = shadowMatrix3 * vec4(worldPos, 1.0);
                    vec3 sc3 = clip3.xyz / clip3.w;
                    vec2 uv3 = vec2(sc3.x * 0.5 + 0.5, 0.5 - sc3.y * 0.5);
                    if (uv3.x >= 0.0 && uv3.x <= 1.0 && uv3.y >= 0.0 && uv3.y <= 1.0 &&
                        sc3.z >= 0.0 && sc3.z <= 1.0) {
                        vec2 moments = texture(sampler2D(shadowMoments3, defaultSampler), uv3).xy;
                        shadowVisibility = chebyshevVisibility(moments, sc3.z);
                    }
                    // 全部越界：lit
                }
            }
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
    // （z+10 = 材质消融档，不是诊断视图——消融走完整光照，此处必须放行）
    if (shadowParams.z > 0.5 && shadowParams.z < 10.5) {
        vec3 diagnostic = normalEnc;
        if (shadowParams.z == 2.0) diagnostic = vec3(NdotL);
        if (shadowParams.z == 3.0) diagnostic = vec3(shadowVisibility);
        if (shadowParams.z == 4.0) diagnostic = albedo;
        if (shadowParams.z == 5.0) {
            // sun-audit P2: IBL ambient 项直出（环境镜面解耦 A/B 测量）。
            // IBL_DIFFUSE_INTENSITY=0 时 = 纯环境镜面；可见性处理与主路径
            // 同分支（stand-in 地板或 PRIMAL_SPEC_OCC 的 GTAO 遮挡）。
            vec3 dFil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
            vec3 dR = reflect(-V, N);
            vec3 dPre = SamplePrefilteredEnvironment(dR, roughness);
            vec2 dBrdf = texture(sampler2D(brdfLUT, defaultSampler),
                                 vec2(max(dot(N, V), 0.0), roughness)).rg;
            vec3 dSpecIBL = dPre * (dFil * dBrdf.x + dBrdf.y);
#if defined(PRIMAL_PROBE_OCC)
            float dPv = probeSpecVisibility(worldPos);
            float dVis2 = dPv < 0.0 ? mix(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) : dPv;
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY * dVis2;
#elif defined(PRIMAL_SPEC_OCC)
            const float dOcc = clamp(pow(max(dot(N, V), 0.0) + ao, 4.0) - 1.0 + ao, 0.0, 1.0);
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY * dOcc;
#else
            diagnostic = dSpecIBL * IBL_SPECULAR_INTENSITY
                       * mix(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) * ao;
#endif
        }
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

    // P4.1: 布帘透光——阴影区背光透射合成。ShadowFilter 输出的透光强度
    // 已含厚度衰减与背面因子；shadowParams.w = 全局透光系数
    // （PRIMAL_TONE_TRANSLUCENCY，默认 0 = 关闭，画面零变化）。
    // P2-B: 再乘 ORM.a 材质掩码——只有标注为透光的薄布料实例生效，
    // 石墙系数严格为 0（不再依赖全局开关单独兜底材质区分）。
    {
        float trans = texture(sampler2D(translucentTex, defaultSampler), inUv).r
                    * shadowParams.w * transmissionMask;
        Lo += albedo * lightColor.rgb * trans * DIRECT_INTENSITY;
    }

    outColor = vec4(Lo, 1.0);
    if (ambientLight.a < 0.5) return;

    // T4.6.5 part 37: IBL ambient (image-based lighting).
    // Mirror Metal fragmentLighting_v3 lines 270-287. Cube samples use the
    // shared defaultSampler (linear + repeat — fine for env maps). Prefilter
    // uses textureLod with roughness across the actual bound mip range.
    vec3 Fil = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kSil = Fil;
    vec3 kDil = (vec3(1.0) - kSil) * (1.0 - metallic);

    vec3 irradiance = texture(samplerCube(irradianceMap, defaultSampler), N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = SamplePrefilteredEnvironment(R, roughness);
    vec2 brdf = texture(sampler2D(brdfLUT, defaultSampler),
                        vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (Fil * brdf.x + brdf.y);

    if (ambientLight.a > 1.5) {
        // The environment BRDF is separated from geometric visibility. The
        // latter is measured along the reflection lobe before post composite.
        // Keep only material micro-occlusion here: no sun-shadow/SSAO proxy.
        outEnvironmentSpecular = vec4(specularIBL * IBL_SPECULAR_INTENSITY * clamp(orm.r,0.0,1.0), 1.0);
        return;
    }

    // T4.6.5 part 40.3: scale overall IBL down, and further crush ambient in
    // shadowed pixels so the shadow side reads darker against lit regions.
    // P2-A: per-branch gating — diffuse off (DDGI owns ambient), specular on.
    // P2（reaudit §4.6）: 镜面/漫反射分支使用各自的阴影地板——太阳阴影不再
    // 直接决定环境镜面可见性。
    float ambientVisibility = mix(SHADOW_AMBIENT_SCALE, 1.0, shadowVisibility);
    float specularVisibility = mix(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility);
#if defined(PRIMAL_PROBE_OCC)
    // sun-audit P2 第二步（PRIMAL_SPEC_OCC=2 → PROBE_OCC 编译变体）：以
    // DDGI 探针 Chebyshev 可见性（真实方向几何遮挡）替换 GTAO 饱和近似
    // ——消除高 N·V+ao 处 specOcc≡1 的局限。探针不可用时回退 stand-in。
    float pv = probeSpecVisibility(worldPos);
    specularVisibility = pv < 0.0 ? mix(SHADOW_SPECULAR_SCALE, 1.0, shadowVisibility) : pv;
    vec3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility) * ao
                 + specularIBL * IBL_SPECULAR_INTENSITY * specularVisibility;
#elif defined(PRIMAL_SPEC_OCC)
    // sun-audit P2 环境镜面解耦第一步（PRIMAL_SPEC_OCC 变体，编译期 -D）：
    // 以屏幕空间可见性（GTAO AO）+ N·V 的 GTAO 镜面遮挡近似**替换**太阳
    // shadowVisibility 代理——固定天空旋转太阳时环境镜面不再仅因太阳入
    // 阴影而骤降（审计验收线）。直射镜面（Cook-Torrance specular）仍用
    // shadowVisibility（直射本应被太阳阴影遮蔽）。遮挡公式自带 ao 项，
    // 故本分支不再外乘 ao。
    const float specOcc = clamp(pow(max(dot(N, V), 0.0) + ao, 4.0) - 1.0 + ao, 0.0, 1.0);
    vec3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility) * ao
                 + specularIBL * IBL_SPECULAR_INTENSITY * specOcc;
#else
    vec3 ambient = (kDil * diffuseIBL * IBL_DIFFUSE_INTENSITY * ambientVisibility
                  + specularIBL * IBL_SPECULAR_INTENSITY * specularVisibility)
                 * ao;
#endif

    outColor = vec4(Lo, 1.0);
    outEnvironmentSpecular = vec4(ambient, 1.0);
}
