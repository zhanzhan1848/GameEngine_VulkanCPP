/**
 * @file DDGITraceRays.metal
 * @brief Lumen DDGI - Probe ray tracing through GlobalSDF
 *
 * Each thread traces one ray from one probe through the GlobalSDF volume.
 * On hit, projects the hit point to screen space and samples the previous
 * frame's lit scene color (direct + shadow + albedo) as radiance.
 * Falls back to analytical NdotL if prev_frame_color is unavailable.
 * On miss, uses sky color.
 *
 * Dispatch: (ProbeCountTotal * RaysPerProbe, 1, 1)
 * ThreadGroupSize: (64, 1, 1)
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
    float3 local = pos - origin;
    return local.x >= 0.0f && local.x < extent.x &&
           local.y >= 0.0f && local.y < extent.y &&
           local.z >= 0.0f && local.z < extent.z;
}

// Sample the best available SDF cascade at a position
static float sampleBestSDF(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant DDGIVolumeData& vol)
{
    float d = 1e10f;
    if (vol.SdfCascadeCount > 0 &&
        isInsideCascade(pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz))
        d = min(d, sampleSDFCascade(sdf0, pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz));
    if (vol.SdfCascadeCount > 1 &&
        isInsideCascade(pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz))
        d = min(d, sampleSDFCascade(sdf1, pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz));
    if (vol.SdfCascadeCount > 2 &&
        isInsideCascade(pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz))
        d = min(d, sampleSDFCascade(sdf2, pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz));
    return d;
}

// ============================================================================
// Sphere Tracing
// ============================================================================

struct SDFHitResult {
    bool   hit;
    float3 position;
    float  distance;
    float3 normal;
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
    result.normal = float3(0.0f);

    float t = 0.0f;
    float minStep = 0.01f;

    for (uint step = 0; step < DDGI_MAX_SDF_STEPS; ++step) {
        float3 pos = rayOrigin + rayDir * t;

        float minDist = 1e10f;
        bool insideAny = false;

        if (vol.SdfCascadeCount > 0 &&
            isInsideCascade(pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf0, pos, vol.SdfOrigins[0].xyz, vol.SdfExtents[0].xyz));
            insideAny = true;
        }
        if (vol.SdfCascadeCount > 1 &&
            isInsideCascade(pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf1, pos, vol.SdfOrigins[1].xyz, vol.SdfExtents[1].xyz));
            insideAny = true;
        }
        if (vol.SdfCascadeCount > 2 &&
            isInsideCascade(pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf2, pos, vol.SdfOrigins[2].xyz, vol.SdfExtents[2].xyz));
            insideAny = true;
        }

        if (!insideAny) {
            uint coarsest = min(2u, vol.SdfCascadeCount - 1);
            t += vol.SdfExtents[coarsest].x * 0.1f;
            if (t > maxDist) break;
            continue;
        }

        float hitThreshold = vol.SdfVoxelSizes[0].x * 0.5f;
        if (minDist < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = t;

            // Compute surface normal from SDF gradient (central differences)
            float eps = max(vol.SdfVoxelSizes[0].x, 0.01f);
            float3 gradient = float3(
                sampleBestSDF(pos + float3(eps, 0, 0), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - float3(eps, 0, 0), sdf0, sdf1, sdf2, vol),
                sampleBestSDF(pos + float3(0, eps, 0), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - float3(0, eps, 0), sdf0, sdf1, sdf2, vol),
                sampleBestSDF(pos + float3(0, 0, eps), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - float3(0, 0, eps), sdf0, sdf1, sdf2, vol)
            );
            result.normal = normalize(gradient);
            return result;
        }

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
    // Previous frame lit scene color (direct + shadow + albedo)
    texture2d<float, access::sample> prev_frame_color [[texture(3)]],

    // Global shader data (matrices)
    device GlobalShaderData& gd [[buffer(0)]],

    // DDGI volume data (includes light direction + color)
    constant DDGIVolumeData& volume [[buffer(1)]],

    // Output: ray data storage buffer
    device DDGIRayData* ray_buffer [[buffer(2)]]
)
{
    uint totalRays = volume.ProbeCountTotal * volume.RaysPerProbe;
    if (global_id >= totalRays) return;

    uint probeIdx = global_id / volume.RaysPerProbe;
    uint rayIdx   = global_id % volume.RaysPerProbe;

    uint3 gc = ddgiProbeGridCoord(probeIdx, volume.ProbeCounts);
    float3 probePos = ddgiProbeWorldPos(gc, volume.ProbeOrigin, volume.ProbeSpacing);

    float3 rayDir = ddgiRayDirection(rayIdx, volume.RaysPerProbe, volume.FrameIndex);

    SDFHitResult hit = traceSDF(
        probePos, rayDir, volume.RayMaxDistance,
        sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, volume);

    DDGIRayData result;

    if (hit.hit) {
        result.radiance_and_dist.w = hit.distance;

        // Project hit point to screen space to sample prev frame lit color.
        // MUST use previous frame's VP: prev_frame_color was rendered with prev camera,
        // so the hit point must be projected through prev VP to find the correct texel.
        float4 clipPos = gd.PreviousViewProjection * float4(hit.position, 1.0f);
        float3 ndc = clipPos.xyz / clipPos.w;

        // Check if hit point is on screen
        bool onScreen = ndc.x >= -1.0f && ndc.x <= 1.0f &&
                        ndc.y >= -1.0f && ndc.y <= 1.0f &&
                        ndc.z >= 0.0f  && ndc.z <= 1.0f;

        if (onScreen && prev_frame_color.get_width() > 0) {
            // NDC → UV (Metal: Y flipped)
            float2 screenUV = float2(ndc.x * 0.5f + 0.5f, ndc.y * -0.5f + 0.5f);
            constexpr sampler screenS(coord::normalized, filter::linear, address::clamp_to_edge);
            float3 sceneColor = prev_frame_color.sample(screenS, screenUV).rgb;

            // sceneColor is direct lighting only (no DDGI indirect),
            // so includes correct shadows + albedo with no feedback loop.
            // Scale for energy conservation (single-bounce approximation).
            result.radiance_and_dist.xyz = sceneColor * 0.5f;
        } else {
            // Fallback: analytical direct lighting for off-screen hits
            float3 N = hit.normal;
            float3 L = normalize(volume.LightDirection);
            float NdotL = max(dot(N, L), 0.0f);
            result.radiance_and_dist.xyz = volume.LightColor * NdotL * 0.5f;
        }
    } else {
        // Miss: negative distance signals miss
        result.radiance_and_dist.w = -1.0f;
        result.radiance_and_dist.xyz = DDGI_SKY_COLOR;
    }

    ray_buffer[global_id] = result;
}
