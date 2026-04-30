/**
 * @file ScreenProbeAverage.metal
 * @brief Screen Probe GI - SH2 (L0+L1) accumulation with multi-factor confidence
 *
 * Each threadgroup handles one probe. Each thread reads 1 ray, recomputes
 * ray direction (Fibonacci + orientAroundNormal), evaluates 4 SH basis
 * functions, and accumulates into threadgroup shared memory via parallel
 * reduction.
 *
 * Confidence = hitRatio × radianceConsistency
 *   - hitRatio: how many rays hit geometry (0 = all missed, 1 = all hit)
 *   - radianceConsistency: exp(-variance_of_luminance) — penalizes probes
 *     that see wildly different colors in different directions (likely at
 *     geometric boundaries or seeing through geometry)
 *
 * Output: 4 float4 per probe (interleaved: [sh0, sh1, sh2, sh3])
 *   sh[0].w = confidence (multi-factor)
 *   sh[1..3].w = 1.0 (validity flag)
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

constant float PI = 3.14159265358979323846f;
constant float GOLDEN_RATIO = 1.618033988749895f;
constant uint  AVG_TG_SIZE = 64;

// ============================================================================
// Fibonacci sphere direction + normal alignment (must match TraceRays)
// ============================================================================

static float3 fibonacciDirection(uint rayIndex, uint totalRays) {
    float theta = 2.0f * PI * float(rayIndex) / GOLDEN_RATIO;
    float phi = acos(1.0f - 2.0f * (float(rayIndex) + 0.5f) / float(totalRays));
    return float3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

static float3 orientAroundNormal(float3 dir, float3 normal) {
    float3 up = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

// ============================================================================
// SH basis functions (L0 + L1, 4 coefficients)
// ============================================================================

static float4 shBasis(float3 d) {
    return float4(
        0.282095f,                  // Y0 (L0)
        0.488603f * d.y,           // Y1 (L1, y)
        0.488603f * d.z,           // Y2 (L1, z)
        0.488603f * d.x            // Y3 (L1, x)
    );
}

// ============================================================================
// Main kernel
// ============================================================================

kernel void screen_probe_average(
    // Input: per-ray radiance (gridW * gridH * raysPerProbe * float4)
    // xyz = radiance, w = cosTheta (negative = miss)
    device const float4* rayRadiance   [[buffer(0)]],

    // Output: per-probe SH coefficients (totalProbes * 4 * float4)
    // Layout: [probe0_sh0, probe0_sh1, probe0_sh2, probe0_sh3, probe1_sh0, ...]
    device float4*       probeSHOut    [[buffer(1)]],

    // Constants
    constant AvgConstants& constants   [[buffer(2)]],

    // Probe normals (needed to recompute ray directions)
    device const float4* probeNormals  [[buffer(3)]],

    uint tid      [[thread_index_in_threadgroup]],
    uint group_id [[threadgroup_position_in_grid]])
{
    if (group_id >= constants.totalProbes) return;

    // Shared memory for parallel reduction
    threadgroup float3 sharedSH[4][AVG_TG_SIZE];   // 4 SH bands
    threadgroup float  sharedWeight[AVG_TG_SIZE];   // cosTheta sum
    threadgroup float  sharedLum[AVG_TG_SIZE];      // luminance sum for variance
    threadgroup float  sharedLumSq[AVG_TG_SIZE];    // luminance² sum for variance
    threadgroup float  sharedHit[AVG_TG_SIZE];      // 1.0 if hit, 0.0 if miss

    // Each thread reads exactly 1 ray
    uint rayIdx = group_id * constants.raysPerProbe + tid;
    float4 rayData = rayRadiance[rayIdx];

    if (rayData.w >= 0.0f) {
        // Hit: clamp radiance at source to prevent firefly propagation
        constexpr float MAX_RAY_RADIANCE = 5.0f;
        float rayLum = dot(rayData.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        if (rayLum > MAX_RAY_RADIANCE) {
            rayData.rgb *= MAX_RAY_RADIANCE / rayLum;
        }

        // Hit: compute SH basis and accumulate
        float4 normData = probeNormals[group_id];
        float3 probeNormal = normalize(normData.xyz);

        float3 localDir = fibonacciDirection(tid, constants.raysPerProbe);
        float3 rayDir = orientAroundNormal(localDir, probeNormal);

        float4 Y = shBasis(rayDir);

        float3 weightedRadiance = rayData.rgb * rayData.w;
        sharedSH[0][tid] = weightedRadiance * Y.x;
        sharedSH[1][tid] = weightedRadiance * Y.y;
        sharedSH[2][tid] = weightedRadiance * Y.z;
        sharedSH[3][tid] = weightedRadiance * Y.w;
        sharedWeight[tid] = rayData.w;

        // Luminance for variance computation
        float lum = dot(rayData.rgb, float3(0.2126f, 0.7152f, 0.0722f));
        sharedLum[tid] = lum;
        sharedLumSq[tid] = lum * lum;
        sharedHit[tid] = 1.0f;
    } else {
        // Miss: contribute nothing
        sharedSH[0][tid] = float3(0.0f);
        sharedSH[1][tid] = float3(0.0f);
        sharedSH[2][tid] = float3(0.0f);
        sharedSH[3][tid] = float3(0.0f);
        sharedWeight[tid] = 0.0f;
        sharedLum[tid] = 0.0f;
        sharedLumSq[tid] = 0.0f;
        sharedHit[tid] = 0.0f;
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Parallel reduction (log2(64) = 6 steps)
    for (uint stride = AVG_TG_SIZE >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            sharedSH[0][tid] += sharedSH[0][tid + stride];
            sharedSH[1][tid] += sharedSH[1][tid + stride];
            sharedSH[2][tid] += sharedSH[2][tid + stride];
            sharedSH[3][tid] += sharedSH[3][tid + stride];
            sharedWeight[tid] += sharedWeight[tid + stride];
            sharedLum[tid]    += sharedLum[tid + stride];
            sharedLumSq[tid]  += sharedLumSq[tid + stride];
            sharedHit[tid]    += sharedHit[tid + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    // Thread 0 writes the final SH coefficients + multi-factor confidence
    if (tid == 0) {
        uint base = group_id * 4;
        float hitCount = sharedHit[0];
        float totalWeight = sharedWeight[0];

        if (totalWeight > 0.0f && hitCount > 0.0f) {
            float invWeight = 1.0f / totalWeight;

            // Hit ratio: how many rays hit geometry
            float hitRatio = hitCount / float(constants.raysPerProbe);

            // Radiance variance: Var(X) = E[X²] - E[X]²
            float avgLum = sharedLum[0] / hitCount;
            float avgLumSq = sharedLumSq[0] / hitCount;
            float lumVariance = max(avgLumSq - avgLum * avgLum, 0.0f);

            // Radiance consistency: low variance → high consistency
            // Scale factor: variance of 1.0 → consistency ≈ 0.02
            float radianceConsistency = exp(-lumVariance * 4.0f);

            // Multi-factor confidence
            float confidence = hitRatio * radianceConsistency;

            probeSHOut[base + 0] = float4(sharedSH[0][0] * invWeight, confidence);
            probeSHOut[base + 1] = float4(sharedSH[1][0] * invWeight, 1.0f);
            probeSHOut[base + 2] = float4(sharedSH[2][0] * invWeight, 1.0f);
            probeSHOut[base + 3] = float4(sharedSH[3][0] * invWeight, 1.0f);
        } else {
            probeSHOut[base + 0] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 1] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 2] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 3] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}
