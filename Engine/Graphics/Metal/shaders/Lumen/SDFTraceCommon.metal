#ifndef SDF_TRACE_COMMON_METAL
#define SDF_TRACE_COMMON_METAL

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Constants
// ============================================================================

#ifndef SDF_MAX_STEPS
#define SDF_MAX_STEPS 4
#endif

constant float SDF_RELAXATION_OMEGA   = 1.2f;
constant float SDF_HIT_THRESHOLD_FACTOR = 0.5f;   // voxelSize * this
constant float SDF_MIN_STEP_FACTOR    = 0.25f;     // voxelSize * this
constant float SDF_RELAXATION_GUARD   = 4.0f;      // relax when d > threshold * this

// ============================================================================
// SDF Hit Result
// ============================================================================

struct SDFHitResult {
    bool   hit;
    float3 position;
    float  distance;
};

// ============================================================================
// Cascade helpers
// ============================================================================

static bool isInsideCascade(float3 pos, float3 origin, float3 extent) {
    float3 local = pos - origin;
    return local.x >= 0.0f && local.x < extent.x &&
           local.y >= 0.0f && local.y < extent.y &&
           local.z >= 0.0f && local.z < extent.z;
}

static float sampleSDFCascade(texture3d<float, access::sample> sdfTexture,
                               float3 worldPos,
                               float3 cascadeOrigin,
                               float3 cascadeExtent) {
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (uvw.x < 0.0f || uvw.x > 1.0f ||
        uvw.y < 0.0f || uvw.y > 1.0f ||
        uvw.z < 0.0f || uvw.z > 1.0f) {
        return 1e10f;
    }
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    return sdfTexture.sample(s, uvw).r;
}

// ============================================================================
// else-if cascade selection — 1 texture3D read per call (Apple Silicon safe)
// Finest cascade first for best accuracy.
// ============================================================================

static float sampleBestSDF_elseIf(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    float4 origin0, float4 origin1, float4 origin2,
    float4 extent0, float4 extent1, float4 extent2,
    uint cascadeCount)
{
    if (cascadeCount > 0 &&
        isInsideCascade(pos, origin0.xyz, extent0.xyz))
        return sampleSDFCascade(sdf0, pos, origin0.xyz, extent0.xyz);
    else if (cascadeCount > 1 &&
        isInsideCascade(pos, origin1.xyz, extent1.xyz))
        return sampleSDFCascade(sdf1, pos, origin1.xyz, extent1.xyz);
    else if (cascadeCount > 2 &&
        isInsideCascade(pos, origin2.xyz, extent2.xyz))
        return sampleSDFCascade(sdf2, pos, origin2.xyz, extent2.xyz);
    return 1e10f;
}

// ============================================================================
// Relaxed sphere tracing — 4 steps, omega=1.2, adaptive threshold
// ============================================================================

static SDFHitResult traceSDF_relaxed(
    float3 rayOrigin,
    float3 rayDir,
    float  maxDist,
    float  finestVoxelSize,
    float  fallbackStride,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    float4 origin0, float4 origin1, float4 origin2,
    float4 extent0, float4 extent1, float4 extent2,
    uint   cascadeCount)
{
    SDFHitResult result;
    result.hit = false;
    result.position = rayOrigin;
    result.distance = maxDist;

    float hitThreshold = finestVoxelSize * SDF_HIT_THRESHOLD_FACTOR;
    float minStep = finestVoxelSize * SDF_MIN_STEP_FACTOR;
    float t = 0.0f;

    for (uint step = 0; step < SDF_MAX_STEPS; ++step) {
        float3 pos = rayOrigin + rayDir * t;

        // else-if cascade selection — 1 texture3D read
        float d = sampleBestSDF_elseIf(pos, sdf0, sdf1, sdf2,
                                       origin0, origin1, origin2,
                                       extent0, extent1, extent2,
                                       cascadeCount);

        // Outside all cascades — use fallback stride
        if (d >= 1e9f) {
            t += fallbackStride;
            if (t > maxDist) break;
            continue;
        }

        // Hit detection
        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = t;
            break;
        }

        // Relaxed sphere tracing: overstep when far, conservative when close
        float advance;
        if (d > hitThreshold * SDF_RELAXATION_GUARD) {
            advance = d * SDF_RELAXATION_OMEGA;
        } else {
            advance = max(d, minStep);
        }
        t += advance;
        if (t > maxDist) break;
    }

    return result;
}

#endif // SDF_TRACE_COMMON_METAL
