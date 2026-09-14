/**
 * @file RadianceCascadeUpdate.metal
 * @brief T3.5 + 2026-09-10 SH-L1 upgrade: Radiance cascade update — inject DDGI
 *        probe radiance into a 32³ world-space cascade
 *        (parity: Vulkan/Shaders/Lumen/RadianceCascadeUpdate.comp)
 *
 * Each texel = one coarse world position; trilinear-sample the 8 nearest DDGI
 * probes and store the aggregated SH L0 + L1 coefficients (4 RGBA16F textures:
 * RGB = raw SH coefficient color, A = validity), each EMA-blended with the
 * previous frame's cascade (temporal_weight engages from frame 2 so the first
 * two frames publish unfiltered data). Far-field rays evaluate directionally
 * in the finalize kernel with the canonical DDGI L0+L1 basis — the old 9→1
 * (DC-only) downgrade is gone.
 *
 * Bindings follow the ScreenProbeGIPass Metal convention (overlapping
 * texture/buffer namespaces, params via SetComputeBytes(3)):
 *   texture(0..3) cascade out L0, L1x, L1y, L1z  (RGBA16F, access::write)
 *   texture(4..7) cascade prev L0, L1x, L1y, L1z (RGBA16F, access::read)
 *   buffer(2)  DDGI irradiance — flat 9-vec4-per-probe storage buffer,
 *              coefficients 0..3 (L0 + L1xyz) at [pIdx * 9 + 0..3]; the L2
 *              tail (coeffs 4..8) is deliberately not propagated.
 *   buffer(3)  CascadeParams (96B, setBytes)
 *
 * Dispatch: (32/4)³ = 8³ groups, threadgroup (4,4,4) — matches the .comp's
 * local_size. texture3d<float, access::read>::read() does NOT clamp
 * coordinates on Apple GPUs, so the prev reads clamp manually.
 */

#include <metal_stdlib>
using namespace metal;

// Must match the Vulkan CascadeParams std140 layout (96B).
struct CascadeParams {
    float4 origin_spacing;        // xyz = cascade origin (world), w = texel spacing
    float4 counts_enabled;        // xyz = cascade counts (32,32,32), w = enabled (0/1)
    float4 ddgi_origin_spacing;   // xyz = DDGI probe grid origin, w = probe spacing
    float4 ddgi_counts;           // xyz = probe counts per axis
    float  temporal_weight;       // EMA factor for the previous cascade
    float  frame_index;           // global frame counter (EMA gate)
    float  pad0;
    float  pad1;
};

kernel void radianceCascadeUpdate(
    uint3 tid [[thread_position_in_grid]],
    texture3d<float, access::write> cascadeOutL0  [[texture(0)]],
    texture3d<float, access::write> cascadeOutL1x [[texture(1)]],
    texture3d<float, access::write> cascadeOutL1y [[texture(2)]],
    texture3d<float, access::write> cascadeOutL1z [[texture(3)]],
    texture3d<float, access::read>  cascadePrevL0  [[texture(4)]],
    texture3d<float, access::read>  cascadePrevL1x [[texture(5)]],
    texture3d<float, access::read>  cascadePrevL1y [[texture(6)]],
    texture3d<float, access::read>  cascadePrevL1z [[texture(7)]],
    device const float4*            ddgiIrradiance [[buffer(2)]],
    constant CascadeParams&         cp [[buffer(3)]])
{
    if (tid.x >= cascadeOutL0.get_width() ||
        tid.y >= cascadeOutL0.get_height() ||
        tid.z >= cascadeOutL0.get_depth()) return;

    // Coefficient register set: rgb = raw SH coefficient, a = validity.
    float3 sh0  = float3(0.0f);
    float3 sh1x = float3(0.0f);
    float3 sh1y = float3(0.0f);
    float3 sh1z = float3(0.0f);

    if (cp.counts_enabled.w < 0.5f) {
        cascadeOutL0.write(float4(sh0, 1.0f), tid);
        cascadeOutL1x.write(float4(sh1x, 1.0f), tid);
        cascadeOutL1y.write(float4(sh1y, 1.0f), tid);
        cascadeOutL1z.write(float4(sh1z, 1.0f), tid);
        return;
    }

    float3 worldPos = cp.origin_spacing.xyz + (float3(tid) + 0.5f) * cp.origin_spacing.w;

    float3 gp = (worldPos - cp.ddgi_origin_spacing.xyz) / cp.ddgi_origin_spacing.w;
    float3 gridMax = cp.ddgi_counts.xyz - 1.0f;
    if (any(gp < float3(0.0f)) || any(gp > gridMax)) {
        // Outside the probe grid: keep the sky-floor ambient so far-cache
        // consumers don't fall to black. Stored as a RAW L0 coefficient now,
        // so divide by C0 — the finalize evaluation multiplies by C0 again,
        // netting the same 0.03 radiance (constant matches the .comp).
        const float C0 = 0.28209479f;
        cascadeOutL0.write(float4(float3(0.03f, 0.03f, 0.035f) / C0, 1.0f), tid);
        cascadeOutL1x.write(float4(0.0f, 0.0f, 0.0f, 1.0f), tid);
        cascadeOutL1y.write(float4(0.0f, 0.0f, 0.0f, 1.0f), tid);
        cascadeOutL1z.write(float4(0.0f, 0.0f, 0.0f, 1.0f), tid);
        return;
    }

    float3 f = clamp(fract(gp), float3(0.0f), float3(1.0f));
    int3 b0 = clamp(int3(floor(gp)), int3(0), int3(cp.ddgi_counts.xyz) - 1);
    int3 b1 = min(b0 + int3(1), int3(cp.ddgi_counts.xyz) - 1);

    float w[8];
    w[0] = (1.0f-f.x)*(1.0f-f.y)*(1.0f-f.z);
    w[1] = f.x*(1.0f-f.y)*(1.0f-f.z);
    w[2] = (1.0f-f.x)*f.y*(1.0f-f.z);
    w[3] = f.x*f.y*(1.0f-f.z);
    w[4] = (1.0f-f.x)*(1.0f-f.y)*f.z;
    w[5] = f.x*(1.0f-f.y)*f.z;
    w[6] = (1.0f-f.x)*f.y*f.z;
    w[7] = f.x*f.y*f.z;

    int3 pc[8];
    pc[0] = int3(b0.x, b0.y, b0.z); pc[1] = int3(b1.x, b0.y, b0.z);
    pc[2] = int3(b0.x, b1.y, b0.z); pc[3] = int3(b1.x, b1.y, b0.z);
    pc[4] = int3(b0.x, b0.y, b1.z); pc[5] = int3(b1.x, b0.y, b1.z);
    pc[6] = int3(b0.x, b1.y, b1.z); pc[7] = int3(b1.x, b1.y, b1.z);

    float wsum = 0.0f;
    for (int p = 0; p < 8; ++p) {
        if (w[p] < 0.01f) continue;
        uint pIdx = uint(pc[p].x) + uint(cp.ddgi_counts.x) * (uint(pc[p].y) + uint(cp.ddgi_counts.y) * uint(pc[p].z));
        uint base = pIdx * 9u;
        sh0  += ddgiIrradiance[base + 0u].xyz * w[p];
        sh1x += ddgiIrradiance[base + 1u].xyz * w[p];
        sh1y += ddgiIrradiance[base + 2u].xyz * w[p];
        sh1z += ddgiIrradiance[base + 3u].xyz * w[p];
        wsum += w[p];
    }
    if (wsum > 0.0f) {
        sh0  /= wsum;
        sh1x /= wsum;
        sh1y /= wsum;
        sh1z /= wsum;
    }

    // Per-coefficient temporal EMA. access::read on texture3d does not clamp
    // coordinates — guard manually.
    uint3 prevSize = uint3(cascadePrevL0.get_width(), cascadePrevL0.get_height(), cascadePrevL0.get_depth());
    uint3 prevCoord = min(tid, prevSize - 1u);
    float tw = (cp.frame_index >= 2.0f) ? cp.temporal_weight : 0.0f;
    sh0  = mix(sh0,  cascadePrevL0.read(prevCoord).rgb,  tw);
    sh1x = mix(sh1x, cascadePrevL1x.read(prevCoord).rgb, tw);
    sh1y = mix(sh1y, cascadePrevL1y.read(prevCoord).rgb, tw);
    sh1z = mix(sh1z, cascadePrevL1z.read(prevCoord).rgb, tw);

    cascadeOutL0.write(float4(sh0, 1.0f), tid);
    cascadeOutL1x.write(float4(sh1x, 1.0f), tid);
    cascadeOutL1y.write(float4(sh1y, 1.0f), tid);
    cascadeOutL1z.write(float4(sh1z, 1.0f), tid);
}
