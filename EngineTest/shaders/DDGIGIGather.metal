// === DDGI GI Gather Shader (8-Probe Trilinear SH9, Vulkan legacy parity) ===
// T8.13 Vulkan↔Metal sync (2026-08-28): body rewritten to the CURRENT Vulkan
// DDGIGIGather.comp legacy route (C) — the path the Vulkan baseline numbers
// (52.5/46.9/97.2/24.7) were measured on:
//   - plain 8-probe trilinear SH9 (canonical basis, matches the projection)
//   - NO visibility/normal weighting, NO static-sky fallback, NO confidence
//     gate, NO cross-bilateral, NO temporal accumulation (the Vulkan legacy
//     path has none of these; leak safety lives in the probe content)
//   - diffuse output = radiance × π (PROBE_SH_RAD_TO_IRRAD) with an absolute
//     sky-leak floor (0.05, 0.05, 0.06)
//   - glossy output = R-lobe radiance blended to the diffuse limit by
//     roughness², alpha = NdotV (DFG happens in FusionComposite)
//   - outside the near grid: dim ambient (0.03, 0.03, 0.035) — the far
//     volume's explicit Metal bindings are deferred in GIGatherModule, so the
//     two-stage far fallback stays Vulkan-only for now.
//
// Apple Silicon structure kept from the previous Metal version: cooperative
// threadgroup staging of the 8 surrounding probes' SH (72 float3) and depth
// (1024 floats), (8,8) tiles, no early returns before the barriers.
//
// PAIRED-CHANGE note (from the Vulkan file): never apply the π factor inside
// SurfaceCacheLightEval's probe gather — its loop gain budget was calibrated
// without it; this correction is for the screen end only.

#include <metal_stdlib>
using namespace metal;

struct StaticProbeData {
    float4 GridOrigin;
    uint3  GridCounts;
    float  Spacing;
    float3 _pad0;
    float4 SkySH[9];
};

#define TG_PROBE_COUNT 8
#define SH_COEFF_COUNT 9
#define TILE_SIZE 8
#define DEPTH_RES 8
#define DEPTH_TEXELS 64
#define DEPTH_FLOATS_PER_PROBE (DEPTH_TEXELS * 2u)

struct TGProbe {
    float3 sh[SH_COEFF_COUNT];
};

// Screen consumers need cosine-weighted irradiance E(N) = π·L̄; the stored SH
// is RADIANCE (mcWeight = 4π/N). Applied at the diffuse output only.
constant float PROBE_SH_RAD_TO_IRRAD = 3.14159265f;

static float3 evalSH9Canonical(threadgroup float3 sh[SH_COEFF_COUNT], float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    // Must exactly match DDGIUpdateIrradiance's projection basis (L1 y/z signs,
    // L2 xy / yz / zonal(3zz-1) / xz / xx-yy slots).
    return sh[0] * 0.28209479f
         + sh[1] * (-0.48860251f * y)
         + sh[2] * ( 0.48860251f * z)
         + sh[3] * (-0.48860251f * x)
         + sh[4] * ( 1.09254843f * y * x)
         + sh[5] * (-1.09254843f * y * z)
         + sh[6] * ( 0.31539157f * (3.0f * z2 - 1.0f))
         + sh[7] * (-1.09254843f * x * z)
         + sh[8] * ( 0.54627422f * (x2 - y2));
}

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],
    uint2 tidInGroup [[thread_position_in_threadgroup]],
    uint2 groupSize [[threads_per_threadgroup]],

    depth2d<float, access::sample> depthTex [[texture(0)]],
    texture2d<float, access::sample> normalTex [[texture(1)]],
    texture2d<float, access::write> outputTex [[texture(2)]],
    texture2d<float, access::sample> historyTex [[texture(3)]],   // unused (Vulkan legacy: temporal disabled)
    texture2d<float, access::sample> ormTex [[texture(4)]],
    texture2d<float, access::write> specularTex [[texture(5)]],

    constant float4* viewCB                [[buffer(0)]],
    constant float4& probeOriginSpacing    [[buffer(1)]],
    constant float4& probeCountsCB         [[buffer(2)]],
    device const float3* staticSkySHBuffer [[buffer(3)]],         // unused (Vulkan legacy route)
    device const float* staticSkyFactor    [[buffer(4)]],         // unused
    constant StaticProbeData& staticProbe  [[buffer(5)]],         // unused
    device const float* confidenceBuffer   [[buffer(6)]],         // unused
    device const float3* irradianceBuffer  [[buffer(7)]],
    device const float* ddgiDepthBuffer    [[buffer(8)]]
) {
    uint width = outputTex.get_width();
    uint height = outputTex.get_height();
    uint localIdx = tidInGroup.y * groupSize.x + tidInGroup.x;

    // ========================================================================
    // Phase 0: Cooperative loading — ALL threads participate, NO early returns
    // ========================================================================
    threadgroup TGProbe tg_probes[TG_PROBE_COUNT];
    threadgroup uint3 tg_cellMin;
    threadgroup bool tg_cellFound;
    threadgroup uint tg_probe_idx[TG_PROBE_COUNT];

    bool inBounds = (tid.x < width && tid.y < height);
    float depth = 0.0f;
    float2 uv = float2(0.0f);
    float3 worldPos = float3(0.0f);
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 gp = float3(0.0f);
    bool active = false;

    if (localIdx == 0u) {
        tg_cellFound = false;
        tg_cellMin = uint3(0u);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (inBounds) {
        uv = (float2(tid) + 0.5f) / float2(width, height);
        depth = depthTex.sample(sampler(address::clamp_to_edge), uv);
        if (depth < 1.0f) {
            float4x4 invVP = float4x4(viewCB[0], viewCB[1], viewCB[2], viewCB[3]);
            float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 worldPosH = invVP * float4(ndc, depth, 1.0f);
            worldPos = worldPosH.xyz / worldPosH.w;

            float3 n = normalTex.sample(sampler(address::clamp_to_edge), uv).xyz;
            n = normalize(n * 2.0f - 1.0f);
            if (isnormal(n.x) && isnormal(n.y) && isnormal(n.z))
                normal = n;

            float3 origin = probeOriginSpacing.xyz;
            float spacing = probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing;

            float3 gridMax = float3(float(probeCountsCB.x - 1.0f),
                                    float(probeCountsCB.y - 1.0f),
                                    float(probeCountsCB.z - 1.0f));
            active = all(gp >= float3(0.0f)) && all(gp <= gridMax);
        }
    }

    if (localIdx < 8u && active && !tg_cellFound) {
        tg_cellMin = uint3(clamp(floor(gp), float3(0.0f),
                                 float3(uint(probeCountsCB.x) - 1u,
                                        uint(probeCountsCB.y) - 1u,
                                        uint(probeCountsCB.z) - 1u)));
        tg_cellFound = true;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint3 counts = uint3(uint(probeCountsCB.x), uint(probeCountsCB.y), uint(probeCountsCB.z));

    // Phase 0a: probe indices for the 8 surrounding probes
    if (localIdx < 8u) {
        uint cx = (localIdx & 1u) ? min(tg_cellMin.x + 1u, counts.x - 1u) : tg_cellMin.x;
        uint cy = (localIdx & 2u) ? min(tg_cellMin.y + 1u, counts.y - 1u) : tg_cellMin.y;
        uint cz = (localIdx & 4u) ? min(tg_cellMin.z + 1u, counts.z - 1u) : tg_cellMin.z;
        tg_probe_idx[localIdx] = cx + cy * counts.x + cz * counts.x * counts.y;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Phase 0b: cooperative SH load (72 float3 over 64 threads)
    {
        uint probe = localIdx / 9u;
        uint shIdx = localIdx % 9u;
        if (probe < TG_PROBE_COUNT) {
            tg_probes[probe].sh[shIdx] = irradianceBuffer[tg_probe_idx[probe] * 9u + shIdx];
        }
    }
    if (localIdx < 8u) {
        uint idx = 64u + localIdx;
        uint probe = idx / 9u;
        uint shIdx = idx % 9u;
        if (probe < TG_PROBE_COUNT && shIdx < SH_COEFF_COUNT) {
            tg_probes[probe].sh[shIdx] = irradianceBuffer[tg_probe_idx[probe] * 9u + shIdx];
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ========================================================================
    // Phase 1: per-pixel output (Vulkan legacy route)
    // ========================================================================
    if (inBounds && !active) {
        // Outside the near grid (or sky): dim ambient (legacy parity).
        outputTex.write(float4(0.03f, 0.03f, 0.035f, 1.0f), tid);
        specularTex.write(float4(0.0f), tid);
    }

    if (active) {
        float spacing = probeOriginSpacing.w;
        float3 fracGp = fract(gp);
        float fx = fracGp.x, fy = fracGp.y, fz = fracGp.z;

        float tw[8];
        tw[0] = (1.0f - fx) * (1.0f - fy) * (1.0f - fz);
        tw[1] = fx           * (1.0f - fy) * (1.0f - fz);
        tw[2] = (1.0f - fx) * fy           * (1.0f - fz);
        tw[3] = fx           * fy           * (1.0f - fz);
        tw[4] = (1.0f - fx) * (1.0f - fy) * fz;
        tw[5] = fx           * (1.0f - fy) * fz;
        tw[6] = (1.0f - fx) * fy           * fz;
        tw[7] = fx           * fy           * fz;

        float nx = normal.x, ny = normal.y, nz = normal.z;

        float3 result = float3(0.0f);
        float totalW = 0.0f;

        // T4.3 glossy: reflection-vector SH9 with the same plain trilinear
        // weights; roughness widens the lobe towards the diffuse limit.
        float3 camPos = viewCB[4].xyz;
        float3 V = normalize(camPos - worldPos);
        float NdotV = clamp(dot(normal, V), 0.05f, 1.0f);
        float3 R = reflect(-V, normal);
        float roughness = clamp(ormTex.sample(sampler(address::clamp_to_edge), uv).g, 0.04f, 1.0f);

        float3 reflResult = float3(0.0f);

        for (uint p = 0u; p < 8u; ++p) {
            if (tw[p] < 0.01f) continue;
            float3 irr = evalSH9Canonical(tg_probes[p].sh, normal);
            result += irr * tw[p];
            totalW += tw[p];

            // Same coefficients along R — no extra device reads.
            float rx = R.x, ry = R.y, rz = R.z;
            float rxy = rx*ry, ryz = ry*rz, rzx = rz*rx;
            float rxx = rx*rx, ryy = ry*ry, rzz = rz*rz;
            reflResult += tg_probes[p].sh[0] * 0.28209479f
                        + tg_probes[p].sh[1] * (-0.48860251f * ry)
                        + tg_probes[p].sh[2] * ( 0.48860251f * rz)
                        + tg_probes[p].sh[3] * (-0.48860251f * rx)
                        + tg_probes[p].sh[4] * ( 1.09254843f * rxy)
                        + tg_probes[p].sh[5] * (-1.09254843f * ryz)
                        + tg_probes[p].sh[6] * ( 0.31539157f * (3.0f * rzz - 1.0f))
                        + tg_probes[p].sh[7] * (-1.09254843f * rzx)
                        + tg_probes[p].sh[8] * ( 0.54627422f * (rxx - ryy));
        }
        if (totalW > 0.0f) {
            result /= totalW;
            reflResult /= totalW;
        }

        // Radiance-unit base for the specular blend (pre-correction scale);
        // the diffuse output gets the radiance→irradiance factor instead.
        float3 radBase = max(result, float3(0.05f, 0.05f, 0.06f));  // sky-leak ambient floor

        // Glossy output: RGB = lobe radiance, A = NdotV (DFG in FusionComposite).
        float3 specIrr = mix(reflResult, radBase, roughness * roughness);
        specularTex.write(float4(specIrr, NdotV), tid);

        float3 diffOut = max(radBase * PROBE_SH_RAD_TO_IRRAD, float3(0.05f, 0.05f, 0.06f));
        outputTex.write(float4(diffOut, 1.0f), tid);
    }

    (void)groupSize;
    (void)ddgiDepthBuffer;
    (void)staticSkySHBuffer;
    (void)staticSkyFactor;
    (void)staticProbe;
    (void)confidenceBuffer;
    (void)historyTex;
}
