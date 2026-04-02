/**
 * @file DDGITraceRays.metal
 * @brief Lumen DDGI Phase 2 - Probe ray tracing through GlobalSDF
 *
 * Each thread traces one ray from one probe through the GlobalSDF volume.
 * On hit, projects hit position to previous frame and samples scene color.
 * On miss, uses sky color.
 *
 * Dispatch: (ProbeCountTotal * RaysPerProbe, 1, 1)
 * ThreadGroupSize: (RaysPerProbe, 1, 1) = (128, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

#include "../CommonTypes.metal"
#include "../CommonFunction.metal"
#include "DDGIVolumeData.metal"

// ============================================================================
// SDF Sampling Helper
// ============================================================================

static float sampleSDFCascade(texture3d<float, access::sample> sdfTexture,
                               float3 worldPos,
                               float3 cascadeOrigin,
                               float3 cascadeExtent)
{
    // Voxelization: texCoord(0,0,0) = cascadeOrigin (minimum corner)
    // So UVW maps [origin, origin + extent] → [0, 1]
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;

    if (uvw.x < 0.0f || uvw.x > 1.0f ||
        uvw.y < 0.0f || uvw.y > 1.0f ||
        uvw.z < 0.0f || uvw.z > 1.0f) {
        return 1e10f;
    }

    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
    return sdfTexture.sample(samp, uvw).r;
}

static bool isInsideCascade(float3 pos, float3 origin, float3 extent)
{
    // Check if pos is within [origin, origin + extent]
    float3 local = pos - origin;
    return local.x >= 0.0f && local.x < extent.x &&
           local.y >= 0.0f && local.y < extent.y &&
           local.z >= 0.0f && local.z < extent.z;
}

// ============================================================================
// Sphere Tracing
// ============================================================================

struct SDFHitResult {
    bool   hit;
    float3 position;
    float  distance;
};

static SDFHitResult traceSDF(
    float3 rayOrigin,
    float3 rayDir,
    float  maxDist,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant DDGIVolumeData& vol)
{
    SDFHitResult result;
    result.hit = false;
    result.position = rayOrigin;
    result.distance = maxDist;

    float t = 0.0f;
    float minStep = 0.01f;

    for (uint step = 0; step < DDGI_MAX_SDF_STEPS; ++step) {
        float3 pos = rayOrigin + rayDir * t;

        float minDist = 1e10f;
        bool insideAny = false;

        // Check cascade 0 (finest)
        if (vol.SdfCascadeCount > 0 &&
            isInsideCascade(pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf0, pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz));
            insideAny = true;
        }
        // Check cascade 1 (medium)
        if (vol.SdfCascadeCount > 1 &&
            isInsideCascade(pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf1, pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz));
            insideAny = true;
        }
        // Check cascade 2 (coarsest)
        if (vol.SdfCascadeCount > 2 &&
            isInsideCascade(pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf2, pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz));
            insideAny = true;
        }

        // Outside all cascades — take a large step
        if (!insideAny) {
            uint coarsest = min(2u, vol.SdfCascadeCount - 1);
            t += vol.SdfExtents[coarsest].x * 0.1f;  // .x from float4
            if (t > maxDist) break;
            continue;
        }

        // Surface hit check using voxel size of finest cascade
        float hitThreshold = vol.SdfVoxelSizes[0].x * 0.5f;  // .x from float4
        if (minDist < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = t;
            return result;
        }

        // Advance by SDF distance
        t += max(minDist, minStep);
        if (t > maxDist) break;
    }

    return result;
}

// ============================================================================
// Main Kernel
// ============================================================================

kernel void ddgi_trace_rays(
    uint global_id [[thread_position_in_grid]],

    // SDF cascade textures
    texture3d<float, access::sample> sdf_cascade_0 [[texture(0)]],
    texture3d<float, access::sample> sdf_cascade_1 [[texture(1)]],
    texture3d<float, access::sample> sdf_cascade_2 [[texture(2)]],

    // Previous frame scene color for hit radiance
    texture2d<float, access::sample> prev_frame_color [[texture(3)]],

    // Previous frame view-projection matrix
    device GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data
    constant DDGIVolumeData& volume [[buffer(1)]],

    // Output: ray data storage buffer
    device DDGIRayData* ray_buffer [[buffer(2)]]
)
{
    uint totalRays = volume.ProbeCountTotal * volume.RaysPerProbe;
    if (global_id >= totalRays) return;

    uint probeIdx = global_id / volume.RaysPerProbe;
    uint rayIdx   = global_id % volume.RaysPerProbe;

    // Probe grid coordinates and world position
    uint3 gc = ddgiProbeGridCoord(probeIdx, volume.ProbeCounts);
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin, volume.ProbeSpacing);

    // Ray direction with frame-index temporal rotation
    float3 rayDir = ddgiFibonacciSphereDir(rayIdx, volume.RaysPerProbe, volume.FrameIndex);

    // Trace through GlobalSDF
    SDFHitResult hit = traceSDF(
        probePos, rayDir, volume.RayMaxDistance,
        sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, volume);

    DDGIRayData result;

    if (hit.hit) {
        result.radiance_and_dist.w = hit.distance;

        // Project hit position to previous frame screen space
        float4 prevClip = gd.PreviousViewProjection * float4(hit.position, 1.0f);
        float3 prevNDC = prevClip.xyz / prevClip.w;

        // Metal NDC → texture UV
        float2 prevUV;
        prevUV.x = prevNDC.x * 0.5f + 0.5f;
        prevUV.y = 0.5f - 0.5f * prevNDC.y;

        sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);

        if (prevUV.x >= 0.0f && prevUV.x <= 1.0f &&
            prevUV.y >= 0.0f && prevUV.y <= 1.0f) {
            result.radiance_and_dist.xyz = prev_frame_color.sample(samp, prevUV).rgb;
        } else {
            result.radiance_and_dist.xyz = DDGI_SKY_COLOR;
        }
    } else {
        // Miss: negative distance signals miss to downstream shaders
        result.radiance_and_dist.w = -1.0f;
        result.radiance_and_dist.xyz = DDGI_SKY_COLOR;
    }

    ray_buffer[global_id] = result;
}
