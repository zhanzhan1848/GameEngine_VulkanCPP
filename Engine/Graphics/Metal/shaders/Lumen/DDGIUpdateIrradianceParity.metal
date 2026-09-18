/**
 * @file DDGIUpdateIrradianceParity.metal
 * @brief T8.13 parity track — faithful Metal port of DDGIUpdateIrradiance.comp
 *
 * Selected at runtime when PRIMAL_DDGI_PARITY=1 (LumenDDGIPass
 * LoadShaderBytecode prefers <name>Parity.metal, falling back to <name>.metal
 * per stage). Purpose: A/B the Vulkan probe-content math on Metal against the
 * engine's DDGIUpdateIrradiance.metal WITHOUT touching either default.
 *
 * Faithful to the comp (differences vs the engine file are all deliberate):
 *   - serial per-probe projection (thread (0,0) of the one-group-per-probe
 *     dispatch does the whole probe; the module's (8,8,1) shape is kept, the
 *     other 63 lanes just exit — the comp assigns one probe per invocation)
 *   - NO probe-space spatial blur (engine adds a 0.04 +x/+y/+z history mix
 *     against checkerboarding; the comp has none)
 *   - NO NaN/Inf history guards, NO freeze mode (DDGI_FREEZE), NO hit-ratio /
 *     stability confidence channels — the comp writes confidence[3] only
 *   - output w = 0 (comp) instead of 1; ramp literal 0.0333 (comp)
 * Unchanged: ray sampler call, distance falloff, SH9 basis, MC weight,
 * first-update publish + young-probe alpha ramp + hysteresis, energy clamps.
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "DDGIVolumeData.metal"

kernel void ddgi_update_irradiance(
    uint2 tid2d [[thread_position_in_threadgroup]],
    uint2 gid2d [[threadgroup_position_in_grid]],

    constant GlobalShaderData& gd [[buffer(0)]],
    constant DDGIVolumeData& vol [[buffer(1)]],
    device const DDGIRayData* rayData [[buffer(2)]],
    device const float4* irradianceHistory [[buffer(3)]],
    device float4* irradianceOutput [[buffer(4)]],
    device const uint* probeUpdateList [[buffer(5)]],
    device float* confidenceBuffer [[buffer(6)]]
)
{
    // comp: one probe per invocation. The Metal module gives this kernel one
    // (8,8,1) group per probe, so lane (0,0) runs the comp body and the rest
    // exit — no cooperative staging, no barriers.
    if (!(tid2d.x == 0u && tid2d.y == 0u)) return;

    uint group_id = gid2d.x;
    if (group_id >= vol.ProbeUpdateCount) return;

    uint probeIdx = probeUpdateList[group_id];
    if (probeIdx >= vol.ProbeCountTotal) return;
    uint rayOffset = probeIdx * vol.RaysPerProbe;

    // comp 141-197: accumulate L0+L1+L2
    float3 shAccum[DDGI_SH_COEFF_COUNT];
    for (uint i = 0u; i < DDGI_SH_COEFF_COUNT; i++)
        shAccum[i] = float3(0.0f);

    for (uint r = 0u; r < vol.RaysPerProbe; r++) {
        DDGIRayData ray = rayData[rayOffset + r];
        float3 radiance = ray.radiance_and_dist.xyz;
        float hitDist = ray.radiance_and_dist.w;

        if (hitDist < 0.0f) {
            // Phase 1.2 孪生同步：逃逸/耗尽不再覆盖成 DDGI_SKY_COLOR——
            // 沿用 finalize 写入 ray_buffer 的天空/SC 回退结果。
        } else {
            // Gentler distance falloff — only attenuate near max distance
            float distWeight = 1.0f - smoothstep(vol.RayMaxDistance * 0.8f, vol.RayMaxDistance, hitDist);
            radiance = radiance * max(distWeight, 0.2f);
        }

        // Same sampler the trace side used for this ray index.
        float3 toLight = normalize(-vol.LightDirection.xyz);
        float4 raySample = ddgiSampleRayDirection(r, vol.RaysPerProbe,
                                                  vol.FrameIndex,
                                                  uint(vol.RaySampleMode), toLight);
        float3 d = raySample.xyz;
        float x = d.x;
        float y = d.y;
        float z = d.z;
        float x2 = x * x;
        float y2 = y * y;
        float z2 = z * z;

        float basis[9];
        basis[0] =  DDGI_SH_C0;
        basis[1] = -DDGI_SH_C1 * y;
        basis[2] =  DDGI_SH_C1 * z;
        basis[3] = -DDGI_SH_C1 * x;
        basis[4] =  DDGI_SH_C2_0 * y * x;
        basis[5] = -DDGI_SH_C2_0 * y * z;
        basis[6] =  DDGI_SH_C2_1 * (3.0f * z2 - 1.0f);
        basis[7] = -DDGI_SH_C2_0 * x * z;
        basis[8] =  DDGI_SH_C2_2 * (x2 - y2);

        // PDF-correct MC weight; mode 0 keeps the legacy uniform expression.
        float mcWeight = (uint(vol.RaySampleMode) == 0u)
            ? (4.0f * 3.14159265f / float(vol.RaysPerProbe))
            : 1.0f / (float(vol.RaysPerProbe) * max(raySample.w, 1e-5f));

        for (uint i = 0u; i < DDGI_SH_COEFF_COUNT; i++) {
            shAccum[i] = shAccum[i] + radiance * basis[i] * mcWeight;
        }
    }

    // comp 199-238: temporal blend with history (9 coeffs as vec4 per probe).
    uint probeBase = probeIdx * 9u;

    uint confidenceBase = probeIdx * 4u;
    float conv = confidenceBuffer[confidenceBase + 3u];
    bool firstUpdate = conv < 0.5f;
    float settledAlpha = clamp(vol.TemporalAlpha, 0.0f, 1.0f);
    float alpha = firstUpdate ? 1.0f : mix(0.35f, settledAlpha, min(conv * 2.0f, 1.0f));

    float3 sh0 = shAccum[0];
    float l1Limit = max(length(sh0) * 3.0f, 0.1f);
    float l2Limit = max(length(sh0) * 2.0f, 0.05f);

    for (uint i = 0u; i < DDGI_SH_COEFF_COUNT; i++) {
        float3 history = irradianceHistory[probeBase + i].xyz;
        float3 filtered = mix(history, shAccum[i], alpha);

        // Energy-aware clamp
        if (i == 0u) {
            float mag = length(filtered);
            if (mag > 10.0f) filtered = filtered * (10.0f / mag);
        } else if (i < 4u) {
            float mag = length(filtered);
            if (mag > l1Limit) filtered = filtered * (l1Limit / mag);
        } else {
            float mag = length(filtered);
            if (mag > l2Limit) filtered = filtered * (l2Limit / mag);
        }

        irradianceOutput[probeBase + i] = float4(filtered, 0.0f);
    }

    // comp 240-243: convergence ramp, 1/30 per update.
    confidenceBuffer[confidenceBase + 3u] = min(conv + 0.0333f, 1.0f);

    (void)gd;
}
