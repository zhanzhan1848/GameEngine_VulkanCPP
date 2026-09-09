/**
 * @file RadianceCascadeUpdate.metal
 * @brief T3.5: Radiance cascade update — inject DDGI probe radiance into a 32³
 *        world-space cascade (parity: Vulkan/Shaders/Lumen/RadianceCascadeUpdate.comp)
 *
 * Each texel = one coarse world position; trilinear-sample the 8 nearest DDGI
 * probes and store the aggregated L0 radiance, EMA-blended with the previous
 * frame's cascade (temporal_weight engages from frame 2 so the first two
 * frames publish unfiltered data).
 *
 * Bindings follow the ScreenProbeGIPass Metal convention (overlapping
 * texture/buffer namespaces, params via SetComputeBytes(3)):
 *   texture(0) cascade out  (RGBA16F, access::write)
 *   texture(1) cascade prev (RGBA16F, access::read)
 *   buffer(2)  DDGI irradiance — flat 9-vec4-per-probe storage buffer, L0 at
 *              [pIdx * 9] (same resource every other consumer binds; there is
 *              no texture3D view of it on Metal).
 *   buffer(3)  CascadeParams (96B, setBytes)
 *
 * Dispatch: (32/4)³ = 8³ groups, threadgroup (4,4,4) — matches the .comp's
 * local_size. texture3d<float, access::read>::read() does NOT clamp
 * coordinates on Apple GPUs, so the prev read clamps manually.
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
    texture3d<float, access::write> cascadeOut  [[texture(0)]],
    texture3d<float, access::read>  cascadePrev [[texture(1)]],
    device const float4*            ddgiIrradiance [[buffer(2)]],
    constant CascadeParams&         cp [[buffer(3)]])
{
    if (tid.x >= cascadeOut.get_width() ||
        tid.y >= cascadeOut.get_height() ||
        tid.z >= cascadeOut.get_depth()) return;

    if (cp.counts_enabled.w < 0.5f) {
        cascadeOut.write(float4(0.0f, 0.0f, 0.0f, 1.0f), tid);
        return;
    }

    float3 worldPos = cp.origin_spacing.xyz + (float3(tid) + 0.5f) * cp.origin_spacing.w;

    float3 gp = (worldPos - cp.ddgi_origin_spacing.xyz) / cp.ddgi_origin_spacing.w;
    float3 gridMax = cp.ddgi_counts.xyz - 1.0f;
    if (any(gp < float3(0.0f)) || any(gp > gridMax)) {
        // Outside the probe grid: keep the sky-floor ambient so far-cache
        // consumers don't fall to black (same constant as the .comp).
        cascadeOut.write(float4(0.03f, 0.03f, 0.035f, 1.0f), tid);
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

    const float C0 = 0.28209479f;
    float3 radiance = float3(0.0f);
    float wsum = 0.0f;
    for (int p = 0; p < 8; ++p) {
        if (w[p] < 0.01f) continue;
        uint pIdx = uint(pc[p].x) + uint(cp.ddgi_counts.x) * (uint(pc[p].y) + uint(cp.ddgi_counts.y) * uint(pc[p].z));
        float3 irr = C0 * ddgiIrradiance[pIdx * 9u].xyz;
        radiance += irr * w[p];
        wsum += w[p];
    }
    if (wsum > 0.0f) radiance /= wsum;

    // access::read on texture3d does not clamp coordinates — guard manually.
    uint3 prevSize = uint3(cascadePrev.get_width(), cascadePrev.get_height(), cascadePrev.get_depth());
    uint3 prevCoord = min(tid, prevSize - 1u);
    float3 prev = cascadePrev.read(prevCoord).rgb;

    float tw = (cp.frame_index >= 2.0f) ? cp.temporal_weight : 0.0f;
    radiance = mix(radiance, prev, tw);

    cascadeOut.write(float4(radiance, 1.0f), tid);
}
