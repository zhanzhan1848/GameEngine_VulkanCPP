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
    float frameSeed;   // F5: same value as camera_position.w (< 0 disables)
    float isMode;      // F2: importance resampling on/off (matches cache_params.y)
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

// F5 (M7): per-frame jitter — byte-equivalent to EncodeLib.glsl's
// spgiJitterHash/spgiJitterRotation (seed via AvgConstants.frameSeed).
static uint spgiJitterHash(uint v) {
    v ^= v >> 16u; v *= 0x7feb352du;
    v ^= v >> 15u; v *= 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

static float3x3 spgiJitterRotation(uint probeIdx, float frameSeed) {
    if (frameSeed < 0.0f) return float3x3(1.0f);
    uint fs = uint(frameSeed);
    uint h1 = spgiJitterHash(probeIdx * 0x9e3779b9u ^ fs);
    uint h2 = spgiJitterHash(h1 ^ 0x68bc21ebu);
    float a1 = float(h1) * (2.0f * PI / 4294967295.0f);
    float a2 = float(h2) * (2.0f * PI / 4294967295.0f);
    float c1 = cos(a1), s1 = sin(a1);
    float c2 = cos(a2), s2 = sin(a2);
    float3x3 rz1(float3( c1, s1, 0.0f), float3(-s1, c1, 0.0f), float3(0.0f, 0.0f, 1.0f));
    float3x3 rxf(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, -1.0f), float3(0.0f, 1.0f, 0.0f));
    float3x3 rz2(float3( c2, s2, 0.0f), float3(-s2, c2, 0.0f), float3(0.0f, 0.0f, 1.0f));
    return rz2 * rxf * rz1;
}

// F2 (M7) 形态 c: importance resampling — byte-equivalent to
// EncodeLib.glsl::spgiImportanceDirection (see TraceRays.metal's copy for
// the full contract; Average regenerates directions to match the traces).
constant uint SPGI_IS_MAX_RAYS = 64u; /* MSL: program scope needs constant address space */

static float3 spgiImportanceDirection(uint rayIdx, uint raysPerProbe,
                                      uint probeIdx, float frameSeed,
                                      float3 probeNormal,
                                      float3 sh0, float3 sh1, float3 sh2, float3 sh3,
                                      thread float& invNq) {
    uint n = min(raysPerProbe, SPGI_IS_MAX_RAYS);
    float3x3 rot = spgiJitterRotation(probeIdx, frameSeed);

    float3 up = fabs(probeNormal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, probeNormal));
    float3 bitangent = cross(probeNormal, tangent);

    float pdf[SPGI_IS_MAX_RAYS];
    float sum = 0.0f;
    for (uint i = 0u; i < n; ++i) {
        float3 local = rot * fibonacciDirection(i, n);
        float3 world = tangent * local.x + bitangent * local.y + probeNormal * local.z;
        float3 irr = sh0 * 0.282095f
                   + sh1 * (0.488603f * world.y)
                   + sh2 * (0.488603f * world.z)
                   + sh3 * (0.488603f * world.x);
        float lum = max(dot(irr, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
        pdf[i] = max(local.z, 0.02f) * (lum + 0.05f);
        sum += pdf[i];
    }

    float u = (float(rayIdx) + 0.5f) / float(n) * sum;
    uint c = n - 1u;
    float acc = 0.0f;
    for (uint i = 0u; i < n; ++i) {
        acc += pdf[i];
        if (u <= acc) { c = i; break; }
    }

    invNq = sum / (float(n) * max(pdf[c], 1e-6f));
    return rot * fibonacciDirection(c, n);
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

    // F2 (M7): previous frame's final probe SH — importance proposal
    device const float4* importanceSH  [[buffer(4)]],

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

        float3 rayDir;
        float invNq;   // already applied to rayData.w by the Finalize kernel
        if (constants.isMode > 0.5f) {
            uint hb = group_id * 4u;
            float4 h0 = importanceSH[hb + 0u];
            float4 h1 = importanceSH[hb + 1u];
            float4 h2 = importanceSH[hb + 2u];
            float4 h3 = importanceSH[hb + 3u];
            rayDir = spgiImportanceDirection(tid, constants.raysPerProbe, group_id,
                                             constants.frameSeed, probeNormal,
                                             h0.rgb, h1.rgb, h2.rgb, h3.rgb, invNq);
            rayDir = orientAroundNormal(rayDir, probeNormal);
        } else {
            float3 localDir = fibonacciDirection(tid, constants.raysPerProbe);
            localDir = spgiJitterRotation(group_id, constants.frameSeed) * localDir;
            rayDir = orientAroundNormal(localDir, probeNormal);
        }

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

            // The reduced coefficients are cosine-weighted hemispherical
            // expectations. Convert to the normalization expected by the
            // standard SH evaluator (L0 * 2pi, L1 * pi).
            constexpr float L0_HEMISPHERE_SCALE = 6.28318530718f; /* MSL: no constant-address locals */
            constexpr float L1_HEMISPHERE_SCALE = 3.14159265359f;

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

            probeSHOut[base + 0] = float4(sharedSH[0][0] * invWeight * L0_HEMISPHERE_SCALE, confidence);
            probeSHOut[base + 1] = float4(sharedSH[1][0] * invWeight * L1_HEMISPHERE_SCALE, 1.0f);
            probeSHOut[base + 2] = float4(sharedSH[2][0] * invWeight * L1_HEMISPHERE_SCALE, 1.0f);
            probeSHOut[base + 3] = float4(sharedSH[3][0] * invWeight * L1_HEMISPHERE_SCALE, 1.0f);
        } else {
            probeSHOut[base + 0] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 1] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 2] = float4(0.0f, 0.0f, 0.0f, 0.0f);
            probeSHOut[base + 3] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
}
