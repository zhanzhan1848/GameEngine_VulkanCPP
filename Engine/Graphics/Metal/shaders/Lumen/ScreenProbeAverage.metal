/**
 * @file ScreenProbeAverage.metal
 * @brief Screen Probe GI - Average per-ray radiance into per-probe radiance
 *
 * Uses threadgroup-based parallel reduction: each threadgroup handles one probe.
 * Thread 0..raysPerProbe-1 each reads exactly 1 ray, then cooperative reduction
 * produces the weighted average. This keeps per-thread buffer reads at 1, well
 * within Apple Silicon limits.
 *
 * REQUIREMENT: ThreadGroupSize must equal raysPerProbe.
 * Dispatch: totalProbes, 1, 1  (one threadgroup per probe)
 */

#include <metal_stdlib>
using namespace metal;

struct AvgConstants {
    uint totalProbes;
    uint raysPerProbe;
};

constant uint AVG_TG_SIZE = 64;

kernel void screen_probe_average(
    // Input: per-ray radiance (gridW * gridH * raysPerProbe * float4)
    device const float4* rayRadiance   [[buffer(0)]],

    // Output: per-probe average radiance (gridW * gridH * float4)
    device float4*       probeAvgOut   [[buffer(1)]],

    // Constants
    constant AvgConstants& constants   [[buffer(2)]],

    uint tid      [[thread_index_in_threadgroup]],
    uint group_id [[threadgroup_position_in_grid]])
{
    if (group_id >= constants.totalProbes) return;

    // Each thread reads exactly 1 ray (1 buffer read per thread)
    uint rayIdx = group_id * constants.raysPerProbe + tid;
    float4 rayData = rayRadiance[rayIdx];

    // Threadgroup shared memory for parallel reduction
    threadgroup float3 sharedRadiance[AVG_TG_SIZE];
    threadgroup float  sharedWeight[AVG_TG_SIZE];

    if (rayData.w >= 0.0f) {
        sharedRadiance[tid] = rayData.rgb * rayData.w;
        sharedWeight[tid] = rayData.w;
    } else {
        sharedRadiance[tid] = float3(0.0f);
        sharedWeight[tid] = 0.0f;
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Parallel reduction (log2(64) = 6 steps)
    for (uint stride = AVG_TG_SIZE >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            sharedRadiance[tid] += sharedRadiance[tid + stride];
            sharedWeight[tid] += sharedWeight[tid + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // Thread 0 writes the final result
    if (tid == 0) {
        if (sharedWeight[0] > 0.0f) {
            probeAvgOut[group_id] = float4(sharedRadiance[0] / sharedWeight[0], 1.0f);
        } else {
            probeAvgOut[group_id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}
