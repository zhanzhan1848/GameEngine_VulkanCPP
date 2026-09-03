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
    device const float4*              ddgi_irradiance   [[buffer(6)]])
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

    // T8.13 sync: reconstruct the texel's surface point from the depth atlas
    // (same recipe as the shadow pass). depth<=0 means never captured — the
    // card midplane is the best guess left. The midplane can sit inside walls
    // for large cards, so the probe gather must run at the real surface point.
    float depth = depth_atlas.read(atlas_uv).r;
    float3 world_pos = cardTexelToWorld(atlas_uv, max(depth, 0.0), cards[card_idx]);

    // Inline per-texel lighting (replaces separate LightCull pass)
    float3 direct_light = float3(0.0);
    uint lightCount = min(params.light_count, 256u);

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

    // 8-probe trilinear gather of full L0-L2 SH, canonical basis (matches
    // DDGIUpdateIrradiance's projection). Returns incoming irradiance at a
    // surface point for the given normal; 0 outside the volume / feed invalid.
    // The stored SH is RADIANCE (mcWeight = 4π/N), so this evaluation
    // under-reads true irradiance by π — DELIBERATE: this function sits inside
    // the SC↔probe feedback loop and its gain budget was calibrated with these
    // units. Screen-side consumers get the ×π correction in DDGIGIGather.
    auto gatherProbeIrradiance = [&](float3 wpos, float3 N, thread bool& inVolume) -> float3 {
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

        float3 acc = float3(0.0f);
        float wsum = 0.0f;
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
            float3 irr = sh[0] * 0.282095f
                       + sh[1] * (-0.488603f * N.y)
                       + sh[2] * ( 0.488603f * N.z)
                       + sh[3] * (-0.488603f * N.x)
                       + sh[4] * ( 1.092548f * N.y * N.x)
                       + sh[5] * (-1.092548f * N.y * N.z)
                       + sh[6] * ( 0.315392f * (3.0f * N.z * N.z - 1.0f))
                       + sh[7] * (-1.092548f * N.x * N.z)
                       + sh[8] * ( 0.546274f * (N.x * N.x - N.y * N.y));
            acc += max(irr, float3(0.0f)) * tw;
            wsum += tw;
        }
        if (wsum <= 0.0f) return float3(0.0f);
        // Clamp bounds the accumulated indirect independently of the feedback
        // loop (collection × albedo must stay < 1 or high-albedo walls diverge).
        return min(acc / wsum, float3(4.0f));
    };

    // Previous frame's lighting at this texel — the self-feedback fallback.
    // The feedback slot is not written on the first frames and Metal heap
    // textures are not zero-initialized either: sanitize NaN/Inf/extreme
    // garbage so multi-bounce feedback cannot latch a poisoned value.
    float3 prev_light = prev_lighting_atlas.read(atlas_uv).rgb;
    if (any(isnan(prev_light)) || any(isinf(prev_light))) prev_light = float3(0.0f);
    prev_light = clamp(prev_light, float3(0.0f), float3(256.0f));
    // T8.13 P1 sync: 0.85 → 0.70. Gain audit found the two coupled loops
    // (probe-internal 0.85 feedback ∘ SC 0.85×albedo) sitting at marginal
    // stability for bright albedo — small transients flipped the SC↔probe
    // equilibrium between a normal and a runaway-bright branch. 0.70 restores
    // margin; equilibrium brightness cost measured at a few percent.
    constexpr float SC_FEEDBACK = 0.70f;

    bool probeInVolume = false;
    float3 E_indirect = gatherProbeIrradiance(world_pos, normal, probeInVolume);
    bool probeFed = params.ddgi_counts_valid.w != 0u;

    // In-volume texels take their bounce from the probes. Outside the probe
    // grid (or with the feed off) the gather is zero, so keep the legacy
    // self-feedback there — otherwise those texels collapse to pure black.
    float3 result = (direct_light + E_indirect) * albedo.rgb
                  + (probeFed && probeInVolume ? float3(0.0f)
                                               : prev_light * albedo.rgb * SC_FEEDBACK)
                  + emissive;
    result = min(result, float3(64.0f));

    // 2026-09-04 闪烁修复：SC atlas 时域 EMA——全量 LightEval 每帧从零重写，
    // 阴影采样/探针采样的逐帧噪声直通 atlas → 探针 → GI 屏幕闪烁。
    // 20% 新 + 80% 历史（~5 帧收敛）抑制高频抖动。
    constexpr float SC_TEMPORAL_ALPHA = 0.2f;
    result = mix(prev_light, result, SC_TEMPORAL_ALPHA);

    lighting_out.write(float4(result, 1.0), atlas_uv);
}
