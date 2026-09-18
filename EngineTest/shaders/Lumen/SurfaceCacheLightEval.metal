/**
 * @file SurfaceCacheLightEval.metal
 * @brief Merged per-card texel lighting (LightCull + LightEval in one pass)
 *
 * Dispatch: 1D, total_texels = sum(card.resolution^2) threads.
 * ThreadGroupSize: (256, 1, 1)
 *
 * Thread mapping:
 *   global_tid → binary search card_dispatch[] → card_idx
 *   → local_texel = tid - card.texel_offset
 *   → atlas_uv = card.atlas_offset + (local_x, local_y)
 *
 * Inline per-texel light culling replaces the old separate LightCull pass.
 */

#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

// C 系列：八面体深度采样 + Chebyshev 可见性（配方 = DDGIGIGather.metal
// 第四轮 route-A 移植版）。DEPTH_FLOATS_PER_PROBE = 8×8 texels ×
// (mean+variance) = 128 floats，与 DDGIUpdateDepth 的写入布局一致。
constant uint SC_DEPTH_RES = 8u;
constant uint SC_DEPTH_TEXELS = 64u;
constant uint SC_DEPTH_FLOATS_PER_PROBE = 128u;

float2 scGatherOctEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 u = d.xy / l1norm;
    float2 s = float2(u.x >= 0.0f ? 1.0f : -1.0f, u.y >= 0.0f ? 1.0f : -1.0f);
    if (d.z < 0.0f) u = (1.0f - abs(float2(u.y, u.x))) * s;
    return u * 0.5f + 0.5f;
}

float2 scSampleProbeDepthMV(uint probeIdx, float3 dirToSurface,
                            device const float* ddgi_depth_buffer) {
    // dir 惯例与屏幕侧一致：传"探针→表面"的负向（即表面→探针的反向），
    // 见 DDGIGIGather.metal sampleNearProbeDepth(pIdx, -toProbe, ...)。
    float2 uv = scGatherOctEncode(dirToSurface);
    uint2 texel = uint2(clamp(uint(uv.x * float(SC_DEPTH_RES)), 0u, SC_DEPTH_RES - 1u),
                        clamp(uint(uv.y * float(SC_DEPTH_RES)), 0u, SC_DEPTH_RES - 1u));
    uint octIdx = texel.y * SC_DEPTH_RES + texel.x;
    uint base = probeIdx * SC_DEPTH_FLOATS_PER_PROBE;
    return float2(ddgi_depth_buffer[base + octIdx],
                  ddgi_depth_buffer[base + SC_DEPTH_TEXELS + octIdx]);
}

float scVisibilityWeight(float distToProbe, float mean, float variance) {
    float dist = max(distToProbe - mean, 0.0f);
    if (variance < 0.0001f) return 1.0f;
    float cheb = variance / (variance + dist * dist);
    return clamp(max(cheb * 2.0f, 0.2f), 0.0f, 1.0f);
}

// 8-probe trilinear gather of full L0-L2 SH, canonical basis (matches
// DDGIUpdateIrradiance's projection). Returns incoming irradiance at a
// surface point for the given normal; 0 outside the volume / feed invalid.
// Raw radiance SH is cosine-convolved per band, then multiplied by pi.
// The returned value is irradiance, matching the screen-side DDGI gather.
// Phase 1b（GI 能量所有权修复）: cos-lobe 评基单源 = Vulkan
// ShaderLib/EvalLib.glsl evalProbeIrradianceSH9（E/π 语义）。MSL 无共享
// include——下方 lumenEvalProbeIrradianceSH9 为行为一致的复制实现（注释互指）。
static inline float3 lumenEvalProbeIrradianceSH9(thread const float3 sh[9], float3 n) {
    const float x = n.x, y = n.y, z = n.z;
    const float xy = x*y, yz = y*z, zx = z*x;
    const float xx = x*x, yy = y*y, zz = z*z;
    return sh[0] * 0.28209479f
         + sh[1] * (-0.48860251f * 0.6666667f * y)   // B4: A1 = 2π/3 (÷π)
         + sh[2] * ( 0.48860251f * 0.6666667f * z)
         + sh[3] * (-0.48860251f * 0.6666667f * x)
         + sh[4] * ( 1.09254843f * 0.25f * xy)       // B4: A2 = π/4 (÷π)
         + sh[5] * (-1.09254843f * 0.25f * yz)
         + sh[6] * ( 0.31539157f * 0.25f * (3.0f*zz - 1.0f))
         + sh[7] * (-1.09254843f * 0.25f * zx)
         + sh[8] * ( 0.54627421f * 0.25f * (xx - yy));
}
float3 gatherProbeIrradiance(float3 wpos, float3 N, thread bool& inVolume,
                             constant FlattenedLightingParams& params,
                             device const float4* ddgi_irradiance,
                             device const float* ddgi_depth_buffer,
                             device const float* ddgi_opensky_buffer) {
    inVolume = false;
    if (params.ddgi_counts_valid.w == 0u) return float3(0.0f);
    float3 org = params.ddgi_origin_spacing.xyz;
    float spacing = max(params.ddgi_origin_spacing.w, 0.01f);
    int3 counts = int3(params.ddgi_counts_valid.xyz);
    float3 gp = (wpos - org) / spacing;
    float3 gridMax = float3(counts - 1);
    if (any(gp < float3(0.0f)) || any(gp > gridMax)) return float3(0.0f);
    inVolume = true;

    int3 b0 = clamp(int3(floor(gp)), int3(0), counts - 1);
    int3 b1 = min(b0 + 1, counts - 1);
    float3 fr = clamp(fract(gp), float3(0.0f), float3(1.0f));

    // C 系列：双累加器 —— accVis 走 Chebyshev+法线+掩埋加权（45%），
    // accLow 只做掩埋降权（55% 低频保底，细薄几何处 Chebyshev 拒探针
    // 不至于塌黑）。配方与屏幕侧 DDGIGIGather 一致。
    // GI 根治 Phase 3.2 孪生：像素 openSky 预计算（8 探针三线性 tw 加权）。
    float pixOpen = 0.0f;
    float pixOpenW = 0.0f;
    if (params.radiosity_gate.w != 0u) {
        for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float tw = mix(1.0f - fr.x, fr.x, float(x))
                     * mix(1.0f - fr.y, fr.y, float(y))
                     * mix(1.0f - fr.z, fr.z, float(z));
            if (tw <= 0.0f) continue;
            int3 pc = int3(mix(float3(b0), float3(b1), float3(float(x), float(y), float(z))));
            uint pidx = uint(pc.x) + uint(counts.x) * (uint(pc.y) + uint(counts.y) * uint(pc.z));
            pixOpen += ddgi_opensky_buffer[pidx] * tw;
            pixOpenW += tw;
        }
        if (pixOpenW > 0.0f) pixOpen /= pixOpenW;
    }
    float3 accVis = float3(0.0f);
    float3 accLow = float3(0.0f);
    float visW = 0.0f;
    float lowW = 0.0f;
    for (int z = 0; z < 2; ++z)
    for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
        float tw = mix(1.0f - fr.x, fr.x, float(x))
                 * mix(1.0f - fr.y, fr.y, float(y))
                 * mix(1.0f - fr.z, fr.z, float(z));
        if (tw <= 0.0f) continue;
        int3 pc = int3(mix(float3(b0), float3(b1), float3(float(x), float(y), float(z))));
        uint pidx = uint(pc.x) + uint(counts.x) * (uint(pc.y) + uint(counts.y) * uint(pc.z));

        float3 sh[9];
        for (int i = 0; i < 9; ++i)
            sh[i] = ddgi_irradiance[pidx * 9u + uint(i)].xyz;
        // Cosine convolution relative to A0=pi; final gather applies pi.
        // Phase 1b: 展开 → lumenEvalProbeIrradianceSH9（EvalLib 单源复制）。
        float3 irr = lumenEvalProbeIrradianceSH9(sh, N);
        irr = max(irr, float3(0.0f));

        float3 probePos = org + float3(pc) * spacing;
        // 表面点沿法线推 5% 间距，避免选中所在墙体内的探针（屏幕侧同款）。
        float3 biasedPos = wpos + N * spacing * 0.05f;
        float3 toProbeVec = probePos - biasedPos;
        float distToProbe = length(toProbeVec);
        float3 toProbe = toProbeVec / max(distToProbe, 1e-5f);

        float2 depthMV = scSampleProbeDepthMV(pidx, -toProbe, ddgi_depth_buffer);
        float insidePenalty = 1.0f;
        float burial = 1.0f;
        if (depthMV.x < spacing * 0.2f) {
            insidePenalty = mix(0.2f, 1.0f, smoothstep(0.0f, 0.2f, depthMV.x / spacing));
            burial = mix(0.1f, 1.0f, smoothstep(0.0f, 0.2f, depthMV.x / spacing));
        }
        float normalWeight = mix(0.2f, 1.0f,
            clamp((dot(N, toProbe) + 0.5f) / 0.7f, 0.0f, 1.0f));
        float nwSoft = max(normalWeight, 0.40f);
        float vw = max(scVisibilityWeight(distToProbe, depthMV.x, depthMV.y), 0.38f);

        // Phase 3.2 孪生: 天空分类衰减——探针露天度 × 像素封闭度。
        float classify = 1.0f;
        if (params.radiosity_gate.w != 0u) {
            float pOpen = ddgi_opensky_buffer[pidx];
            float skyness = smoothstep(0.35f, 0.65f, pOpen);
            float enclosed = 1.0f - smoothstep(0.05f, 0.25f, pixOpen);
            classify = 1.0f - skyness * enclosed * 0.85f;
        }

        accVis += irr * tw * nwSoft * vw * insidePenalty * classify;
        visW   += tw * nwSoft * vw * insidePenalty * classify;
        accLow += irr * tw * burial * classify;
        lowW   += tw * burial * classify;
    }
    float3 result = float3(0.0f);
    if (visW > 0.0f) result += (accVis / visW) * 0.45f;
    if (lowW > 0.0f) result += (accLow / lowW) * 0.55f;
    if (visW <= 0.0f && lowW <= 0.0f) return float3(0.0f);
    // 2026-09-10 十: the eval reconstructs RADIANCE L(d); the texel's physical
    // input is IRRADIANCE E ≈ π·L. π deficit no longer deliberate — Vulkan
    // twin promoted the ×π to default the same day (loop fixed point ×1.7-2.1
    // measured, DC gain 0.19→~0.53 < 1, stable through frame 5000). Matches
    // the screen side's B4 ×π (DDGIGIGather PROBE_SH_RAD_TO_IRRAD).
    result *= 3.14159265f;
    // Clamp bounds the accumulated indirect independently of the feedback
    // loop (collection × albedo must stay < 1 or high-albedo walls diverge).
    // 4.0 → 64.0 with the C2.1 SC direct-scale unification (see Vulkan twin).
    return min(result, float3(64.0f));
}


// T3.3 tile radiosity weight — runtime knob. radiosity_gate.y carries the
// weight as raw float bits (0 = legacy 0.25); C++ packs it from
// PRIMAL_SC_RADIOSITY_W. Vulkan twin: SurfaceCacheLightEval.comp
// scRadiosityWeight().
static inline float scRadiosityWeight(uint bits) {
    return bits != 0u ? as_type<float>(bits) : 0.25f;
}

kernel void surfaceCacheLightEval(
    uint global_tid [[thread_position_in_grid]],

    texture2d<float, access::read>  albedo_atlas    [[texture(0)]],
    texture2d<float, access::read>  normal_atlas    [[texture(1)]],
    texture2d<float, access::read>  emissive_atlas  [[texture(2)]],
    texture2d<float, access::write> lighting_out    [[texture(3)]],
    // T8.13 sync: 2-frame-old lighting (multi-bounce feedback) + depth atlas
    // (surface-point reconstruction for the probe gather).
    texture2d<float, access::read>  prev_lighting_atlas [[texture(4)]],
    texture2d<float, access::read>  depth_atlas         [[texture(5)]],

    constant FlattenedLightingParams& params         [[buffer(1)]],
    constant SurfaceCacheCard*        cards          [[buffer(2)]],
    constant LightInfo*               lights         [[buffer(3)]],
    constant CardDispatchInfo*        card_dispatch  [[buffer(4)]],
    device const float*               shadow_visibility [[buffer(5)]],  // T3.3
    // T8.13 sync: probe-fed indirect — 9 vec4 SH coefficients per probe from
    // the near DDGI volume (previous-frame slot; SC runs before the DDGI
    // update). Dummy-bound when the feed is off (flag-gated in-shader).
    device const float4*              ddgi_irradiance   [[buffer(6)]],
    texture2d<float, access::read>    indirect_out      [[texture(6)]],   // T3.3 tile radiosity
    // C 系列：探针八面体深度（mean+variance per texel，128 floats/probe）——
    // gather 的 Chebyshev 可见性，封锁"受光带探针能量隔墙传给阴影卡片"。
    device const float*               ddgi_depth_buffer [[buffer(7)]],
    // GI 根治 Phase 3.2 孪生: openSky 统计（Metal buffer(8)）
    device const float*               ddgi_opensky_buffer [[buffer(8)]])
{
    if (global_tid >= params.total_texels) return;

    // Binary search: find card whose [texel_offset, texel_offset+texel_count)
    // contains global_tid. O(log N) where N = card_count (typically < 100).
    uint lo = 0;
    uint hi = params.card_count;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1;
        if (card_dispatch[mid].texel_offset + card_dispatch[mid].texel_count <= global_tid)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= params.card_count) return;

    uint local_texel = global_tid - card_dispatch[lo].texel_offset;
    uint res = card_dispatch[lo].resolution;
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_uv = uint2(
        card_dispatch[lo].atlas_offset_x + local_x,
        card_dispatch[lo].atlas_offset_y + local_y);
    uint card_idx = card_dispatch[lo].card_index;

    // Read material data
    float4 albedo = albedo_atlas.read(atlas_uv);
    if (albedo.a < 0.01) return;

    float2 enc_n = normal_atlas.read(atlas_uv).rg;
    float3 normal = octDecode(enc_n);
    float3 emissive = emissive_atlas.read(atlas_uv).rgb;
    // Radiosity Intensity Clamp (UE 0x32): the SC↔probe feedback loop gains
    // from the emissive term every frame — clamp the per-texel contribution
    // so a hot emissive source converges instead of exploding.
    emissive = min(emissive, float3(8.0f));
#ifdef PRIMAL_SC_NO_EMISSIVE
    // C 系列归因旋钮：emissive 直加不查遮挡（T4.4 闭环后生效），断电用于
    // 验证室内平填充是否由 emissive 主导。
    emissive = float3(0.0f);
#endif

    // T8.13 sync: reconstruct the texel's surface point from the depth atlas
    // (same recipe as the shadow pass). depth<=0 means never captured — the
    // card midplane is the best guess left. The midplane can sit inside walls
    // for large cards, so the probe gather must run at the real surface point.
    float depth = depth_atlas.read(atlas_uv).r;
    float3 world_pos = cardTexelToWorld(atlas_uv, max(depth, 0.0), cards[card_idx]);

    // Inline per-texel lighting (replaces separate LightCull pass)
    float3 direct_light = float3(0.0);
    uint lightCount = min(params.light_count, 256u);
#ifdef PRIMAL_SC_NO_DIRECT
    // C 系列归因旋钮：断开 SC 卡片直射项——室内 atlas 若随之塌黑则能量
    // 来自 direct×vis（vis 泄漏 / 卡片朝向），不变则来自 E_ind 探针环自持。
    lightCount = 0u;
#endif

    // T3.3: directional visibility from the shadow pass (atlas-space index).
    uint atlas_size = params.sc_params.atlas_size;
    float dir_visibility = shadow_visibility[atlas_uv.x + atlas_size * atlas_uv.y];

    for (uint li = 0; li < lightCount; ++li) {
        float3 L;
        float attenuation;
        uint lightType = uint(lights[li].direction.w);

        if (lightType == 1) {
            // Directional light: visibility from the SDF shadow pass (T3.3)
            L = -normalize(lights[li].direction.xyz);
            attenuation = dir_visibility;
        } else {
            // Point/spot light: per-texel distance check
            float3 to_light = lights[li].position.xyz - world_pos;
            float dist = length(to_light);
            float radius = lights[li].position.w;

            if (dist > radius) continue;

            L = to_light / max(dist, 0.001);
            float r2 = radius * radius;
            attenuation = max(1.0 - (dist * dist) / r2, 0.0);
        }

        float NdotL = max(dot(normal, L), 0.0);
        direct_light += lights[li].color.xyz * NdotL * attenuation;
    }

    // --------------------------------------------------------------------------------
    // T8.13 sync: probe-fed indirect + 0.70-gain multi-bounce fallback
    // (mirrors SurfaceCacheLightEval.comp).
    // --------------------------------------------------------------------------------


    // Previous frame's lighting at this texel — the self-feedback fallback.
    // The feedback slot is not written on the first frames and Metal heap
    // textures are not zero-initialized either: sanitize NaN/Inf/extreme
    // garbage so multi-bounce feedback cannot latch a poisoned value.
    float3 prev_light = prev_lighting_atlas.read(atlas_uv).rgb;
    if (any(isnan(prev_light)) || any(isinf(prev_light))) prev_light = float3(0.0f);
    prev_light = clamp(prev_light, float3(0.0f), float3(256.0f));
    // Phase 2.2 孪生同步（SurfaceCacheLightEval.comp）: SC_FEEDBACK 0.70 手工
    // 系数删除（连同 ×0.5 半档）——反馈改物理形式 prev_light·ρ·(1/π)，
    // 回路稳态增益 = ρ < 1；T8.13"0.85 上限"改为 DC 增益断言（<0.5）。

    bool probeInVolume = false;
    float3 E_indirect = gatherProbeIrradiance(world_pos, normal, probeInVolume,
                                              params, ddgi_irradiance, ddgi_depth_buffer,
                                              ddgi_opensky_buffer);
    bool probeFed = params.ddgi_counts_valid.w != 0u;
    // WRC 计划 Stream B1（能量单向化）Vulkan 孪生同步：探针馈送改暖启动
    // 引导项——PRIMAL_GI_PROBE_FEED_BOOTSTRAP=1 时因子在 GIWarmup 窗口内
    // 1→0 线性、窗口后恒 0（CPU 端 GIProbeBootstrapFactor 随 params 下发）；
    // 旧语义 factor=1，乘法恒等。
    E_indirect *= params.probe_feed_bootstrap;

    // In-volume texels take their bounce from the probes. Outside the probe
    // grid (or with the feed off) the gather is zero, so keep the legacy
    // self-feedback there — otherwise those texels collapse to pure black.
    // B3: SI convention — the atlas stores outgoing RADIANCE
    // L_out = (albedo/π)·E (Lambert). Consumers (probes, gather) already
    // treat it as radiance; the missing 1/π made every downstream stage π×
    // bright and forced ad-hoc 0.5× knobs downstream.
    constexpr float SC_INV_PI = 0.31830989f;
    // T3.3: SC Radiosity tile probe — the indirect_out grid (trace+resolve,
    // D10) stores per-tile first-bounce outgoing radiance (already albedo-
    // weighted). It replaces the legacy self-feedback as the no-probe-feed
    // bounce source and adds a low-frequency supplement in-probe-volume.
    float3 tileRadiance = float3(0.0f);
    if (params.radiosity_gate.x != 0u) {
        // Resolve stores outgoing radiance at full atlas texel coordinates,
        // including this texel's albedo. Tile-grid coordinates address an
        // unrelated surface and may cross card boundaries.
        // B (plan §三.B): A=validity — never-traced/post-reset tiles store
        // explicit zeros with A=0 (Vulkan twin parity).
        float4 tileSample = indirect_out.read(atlas_uv);
        if (tileSample.a > 0.0f) tileRadiance = tileSample.rgb;
    }

    // C3.5 slice 5+6: texel-side ambient floor with sky-openness modulation
    // (occlusion-aware; enclosed crevices keep a 0.25 floor) — Vulkan twin:
    // scSkyOpenness in SurfaceCacheLightEval.comp.
    // WRC 计划 Stream B3（删稳定器）Vulkan B14 对齐：radiosity_solo=1
    // （bootstrap 模式）时地板项关闭——常量注入会让衰减引导项永远到不了
    // 0， defeated 能量单向化。旧语义（solo=0）保留 Metal 现状。
    float skyOpen = 1.0f;
    if (params.radiosity_solo == 0u && params.ddgi_counts_valid.w != 0u && probeInVolume) {
        float3 org = params.ddgi_origin_spacing.xyz;
        float spacing = max(params.ddgi_origin_spacing.w, 0.01f);
        int3 counts = int3(params.ddgi_counts_valid.xyz);
        float3 gp = (world_pos - org) / spacing;
        int3 c0 = clamp(int3(round(gp)), int3(0), counts - 1);
        uint pidx = uint(c0.x) + uint(counts.x) * (uint(c0.y) + uint(counts.y) * uint(c0.z));
        float open = 0.0f;
        for (int k = 0; k < 6; ++k) {
            float3 d = (k == 0) ? float3(1,0,0) : (k == 1) ? float3(-1,0,0)
                     : (k == 2) ? float3(0,1,0) : (k == 3) ? float3(0,-1,0)
                     : (k == 4) ? float3(0,0,1) : float3(0,0,-1);
            float2 mv = scSampleProbeDepthMV(pidx, d, ddgi_depth_buffer);
            open += smoothstep(spacing * 0.5f, spacing * 2.0f, mv.x);
        }
        skyOpen = open / 6.0f;
    }
    if (params.radiosity_solo == 0u) {
        E_indirect = max(E_indirect, float3(0.52f, 0.46f, 0.37f) * mix(0.25f, 1.0f, skyOpen));
    }
    // WRC 计划 Stream B2: radiosity_solo=1 时 tile radiosity 是全部 texel
    //（含 volume 外）的唯一常备间接来源——legacy prev_light 自反馈双轨停用
    //（Vulkan 孪生同步）。
    float3 result = (direct_light + E_indirect) * albedo.rgb * SC_INV_PI
                  + (params.radiosity_solo != 0u
                        ? tileRadiance * scRadiosityWeight(params.radiosity_gate.y)
                        : (probeFed && probeInVolume
                              ? tileRadiance * scRadiosityWeight(params.radiosity_gate.y)
                              : tileRadiance + prev_light * albedo.rgb * SC_INV_PI))
                  + emissive;
    result = min(result, float3(64.0f));

    // Match Vulkan: publish this frame's evaluation directly. Blending with
    // prev_lighting_atlas here creates three independent temporal states because
    // the atlas is triple-buffered, observed as an A/B/C brightness cycle.
    lighting_out.write(float4(result, 1.0), atlas_uv);
}
