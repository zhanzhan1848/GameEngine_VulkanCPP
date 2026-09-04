// === DDGI GI Gather — Vulkan route-C parity port (separate track) ===
//
// T8.13 Vulkan↔Metal sync (2026-09-04). This file is a FAITHFUL, line-for-line
// Metal port of DDGIGIGather.comp legacy route (C) — the path the Vulkan
// baseline numbers (52.5/46.9/97.2/24.7) were measured on. It exists so the
// Vulkan gather MATH can be A/B'd on Metal against the engine's own
// DDGIGIGather.metal without touching either default:
//
//   run TestModularPipeline with  PRIMAL_GATHER=DDGIGIGatherParity.metal
//
// Differences vs the engine's DDGIGIGather.metal (all deliberate, all from
// the Vulkan source):
//   - Full SH9 reconstruction (L0+L1+L2, canonical basis, exact comp
//     constants) — the engine file keeps L0+L1 only for buffer-read budget.
//   - texel-exact gbuffer reads (uv→full-res-texel, read()) instead of
//     bilinear sample() — matches the comp's texelFetch.
//   - Per-pixel trilinear cell selection, plain weights, no visibility /
//     normal weighting, no confidence gate, no temporal (route C has none).
//   - diffuse = radiance × π (PROBE_SH_RAD_TO_IRRAD), absolute sky-leak floor
//     (0.05, 0.05, 0.06); glossy = R-lobe radiance blended to the diffuse
//     limit by roughness², alpha = NdotV (DFG in FusionComposite).
//
// NOT ported (binding gap, not math): route C's two-stage far fallback
// (comp bindings 15/16). GIGatherModule does not bind far-volume CB/SSBO on
// Metal yet, and Metal rejects unbound buffer declarations. Until those
// bindings land, outside-grid pixels take route C's dim-ambient tail
// directly — identical to the engine gather's current behaviour.
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

#define SH_COEFF_COUNT 9

// Screen consumers need cosine-weighted irradiance E(N) = π·L̄; the stored SH
// is RADIANCE (mcWeight = 4π/N). Applied at the diffuse output only.
constant float PROBE_SH_RAD_TO_IRRAD = 3.14159265f;

// SH reconstruction coefficients — exact values from the comp.
constant float SH_C0   = 0.28209479f;
constant float SH_C1   = 0.48860251f;
constant float SH_C2_0 = 1.09254843f;  // xy / yz / xz
constant float SH_C2_1 = 0.31539157f;  // 3zz-1 zonal
constant float SH_C2_2 = 0.54627421f;  // xx-yy

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],
    depth2d<float, access::sample> depthTex [[texture(0)]],
    texture2d<float, access::sample> normalTex [[texture(1)]],
    texture2d<float, access::read_write> outputTex [[texture(2)]],
    texture2d<float, access::sample> historyTex [[texture(3)]],   // unused (route C: temporal disabled)
    texture2d<float, access::sample> ormTex [[texture(4)]],
    texture2d<float, access::write> specularTex [[texture(5)]],

    constant float4* viewCB                [[buffer(0)]],   // invViewProj rows 0..3, cameraPos @4
    constant float4& probeOriginSpacing    [[buffer(1)]],   // xyz=origin, w=spacing
    constant float4& probeCountsCB         [[buffer(2)]],   // xyz=counts
    device const float3* staticSkySHBuffer [[buffer(3)]],   // unused (route C)
    device const float* staticSkyFactor    [[buffer(4)]],   // unused (route C)
    constant StaticProbeData& staticProbe  [[buffer(5)]],   // unused (route C)
    device const float* confidenceBuffer   [[buffer(6)]],   // unused (route C)
    device const float3* irradianceBuffer  [[buffer(7)]],   // 9 float3 slots/probe, 16B stride (= comp vec4[])
    device const float* ddgiDepthBuffer    [[buffer(8)]]    // unused (no visibility weighting in route C)
) {
    uint width = outputTex.get_width();
    uint height = outputTex.get_height();

    bool inBounds = (tid.x < width && tid.y < height);
    float depth = 0.0f;
    float2 uv = float2(0.0f);
    float3 worldPos = float3(0.0f);
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 gp = float3(0.0f);
    bool active = false;

    if (inBounds) {
        uv = (float2(tid) + 0.5f) / float2(width, height);
        // comp: texelFetch(gbuffer_depth, ivec2(fullResSize * uv)) — texel
        // exact even when the gather runs below gbuffer resolution.
        uint2 fullTexel = uint2(float2(uint2(depthTex.get_width(), depthTex.get_height())) * uv);
        depth = depthTex.read(fullTexel);
        if (depth < 1.0f) {
            float4x4 invVP = float4x4(viewCB[0], viewCB[1], viewCB[2], viewCB[3]);
            float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 worldPosH = invVP * float4(ndc, depth, 1.0f);
            worldPos = worldPosH.xyz / worldPosH.w;

            uint2 nTexel = uint2(float2(uint2(normalTex.get_width(), normalTex.get_height())) * uv);
            float3 n = normalTex.read(nTexel).xyz;
            n = normalize(n * 2.0f - 1.0f);
            // GLSL has no isnormal(); check components (comp parity).
            if (!isnan(n.x) && !isnan(n.y) && !isnan(n.z))
                normal = n;

            float3 origin = probeOriginSpacing.xyz;
            float spacing = probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing;

            float3 gridMax = float3(probeCountsCB.x - 1.0f,
                                    probeCountsCB.y - 1.0f,
                                    probeCountsCB.z - 1.0f);
            active = all(gp >= float3(0.0f)) && all(gp <= gridMax);
        }
    }

    uint3 counts = uint3(uint(probeCountsCB.x), uint(probeCountsCB.y), uint(probeCountsCB.z));

    // Route C inactive tail. The comp first tries the far coarse volume here
    // (bindings 15/16) — deferred on Metal, see file header.
    if (inBounds && !active) {
        outputTex.write(float4(0.03f, 0.03f, 0.035f, 1.0f), tid);
        specularTex.write(float4(0.0f), tid);
    }

    if (active) {
        // comp 869-891: direct per-pixel trilinear — no cooperative loading.
        uint3 b0 = clamp(uint3(floor(gp)), uint3(0u), counts - 1u);
        uint3 b1 = min(b0 + 1u, counts - 1u);
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

        uint3 pc[8];
        pc[0] = uint3(b0.x, b0.y, b0.z); pc[1] = uint3(b1.x, b0.y, b0.z);
        pc[2] = uint3(b0.x, b1.y, b0.z); pc[3] = uint3(b1.x, b1.y, b0.z);
        pc[4] = uint3(b0.x, b0.y, b1.z); pc[5] = uint3(b1.x, b0.y, b1.z);
        pc[6] = uint3(b0.x, b1.y, b1.z); pc[7] = uint3(b1.x, b1.y, b1.z);

        // Precomputed axis products, comp 900-920 — shared across the two
        // SH9 evaluations (N lobe diffuse + R lobe glossy).
        float nx = normal.x, ny = normal.y, nz = normal.z;
        float nxy = nx*ny, nyz = ny*nz, nzx = nz*nx;
        float nxx = nx*nx, nyy = ny*ny, nzz = nz*nz;

        float3 result = float3(0.0f);
        float totalW = 0.0f;

        // T4.3 glossy: R = reflect(-V, N), same SH9 coefficients; roughness²
        // blends the lobe towards the diffuse limit.
        float3 camPos = viewCB[4].xyz;
        float3 V = normalize(camPos - worldPos);
        float NdotV = clamp(dot(normal, V), 0.05f, 1.0f);
        float3 R = reflect(-V, normal);
        float roughness;
        {
            uint2 oTexel = uint2(float2(uint2(ormTex.get_width(), ormTex.get_height())) * uv);
            roughness = clamp(ormTex.read(oTexel).g, 0.04f, 1.0f);
        }
        float rx = R.x, ry = R.y, rz = R.z;
        float rxy = rx*ry, ryz = ry*rz, rzx = rz*rx;
        float rxx = rx*rx, ryy = ry*ry, rzz = rz*rz;
        float3 reflResult = float3(0.0f);

        for (uint p = 0u; p < 8u; ++p) {
            if (tw[p] < 0.01f) continue;
            uint pIdx = pc[p].x + pc[p].y * counts.x + pc[p].z * counts.x * counts.y;
            float3 sh[9];
            for (uint i = 0u; i < 9u; ++i)
                sh[i] = irradianceBuffer[pIdx * 9u + i];
            // SH9 reconstruction — canonical basis, comp 929-938. Must match
            // DDGIUpdateIrradiance's projection basis: L1 y and L2 yz/xz carry
            // minus signs, band 6 is the C2_1 zonal term.
            float3 irr = sh[0] * SH_C0
                       + sh[1] * (-SH_C1 * ny)
                       + sh[2] * ( SH_C1 * nz)
                       + sh[3] * (-SH_C1 * nx)
                       + sh[4] * ( SH_C2_0 * nxy)
                       + sh[5] * (-SH_C2_0 * nyz)
                       + sh[6] * ( SH_C2_1 * (3.0f*nzz - 1.0f))
                       + sh[7] * (-SH_C2_0 * nzx)
                       + sh[8] * ( SH_C2_2 * (nxx - nyy));
            result += irr * tw[p];
            totalW += tw[p];

            // Same coefficients along R — no extra device reads (comp 942-951).
            reflResult += sh[0] * SH_C0
                        + sh[1] * (-SH_C1 * ry)
                        + sh[2] * ( SH_C1 * rz)
                        + sh[3] * (-SH_C1 * rx)
                        + sh[4] * ( SH_C2_0 * rxy)
                        + sh[5] * (-SH_C2_0 * ryz)
                        + sh[6] * ( SH_C2_1 * (3.0f*rzz - 1.0f))
                        + sh[7] * (-SH_C2_0 * rzx)
                        + sh[8] * ( SH_C2_2 * (rxx - ryy));
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

    (void)ddgiDepthBuffer;
    (void)staticSkySHBuffer;
    (void)staticSkyFactor;
    (void)staticProbe;
    (void)historyTex;
}
