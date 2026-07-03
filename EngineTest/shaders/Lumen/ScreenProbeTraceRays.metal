/**
 * @file ScreenProbeTraceRays.metal
 * @brief Screen Probe GI - Ray tracing from screen-space probes
 *
 * Each thread traces one ray from one probe through the GlobalSDF volume.
 * Uses Fibonacci sphere sampling for uniform ray direction distribution.
 * On hit, samples the previous frame's lit scene color as radiance.
 * On miss, outputs black (DDGI fallback not yet implemented).
 *
 * Dispatch: (gridW * gridH * raysPerProbe + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 *
 * Uses 1D dispatch to avoid Metal validation errors when sharing a compute
 * encoder with other passes that use scalar thread_position_in_grid.
 */

#include <metal_stdlib>
using namespace metal;

#include "SurfaceCacheData.metal"

// ============================================================================
// Constant buffer layout (must match C++ ScreenProbeGlobalData)
// ============================================================================

struct ScreenProbeGlobalData {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float4   camera_position;

    float4   grid_params;     // x=gridW, y=gridH, z=downsample, w=raysPerProbe
    float4   trace_params;    // x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight

    float4   sdf_origins[3];
    float4   sdf_voxel_sizes[3];
    float4   sdf_extents[3];
    float4   sdf_resolutions; // x=res0, y=res1, z=res2, w=cascadeCount

    float4   surface_cache_params; // x=atlasSize, y=cardCount, z=surfaceCacheAvailable(0/1), w=unused
};

// ============================================================================
// Constants
// ============================================================================

constant float PI = 3.14159265358979323846f;
constant float GOLDEN_RATIO = 1.618033988749895f;
// Was 4 — too few for sphere tracing. Bumped to 32 so rays actually reach
// geometry within maxDist instead of reporting miss.
constant uint  MAX_SDF_STEPS = 32;
constant float SDF_HIT_THRESHOLD_FACTOR = 1.5f;

// ============================================================================
// SDF Sampling (same as DDGITraceRays)
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
    sampler samp(coord::normalized, filter::linear, address::clamp_to_edge);
    return sdfTexture.sample(samp, uvw).r;
}

static float sampleBestSDF(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant ScreenProbeGlobalData& global) {
    uint cascadeCount = uint(global.sdf_resolutions.w);

    // Apple Silicon: else-if cascade = 1 texture3D read per step (not 3).
    // Finest cascade first for best accuracy.
    if (cascadeCount > 0 &&
        isInsideCascade(pos, global.sdf_origins[0].xyz, global.sdf_extents[0].xyz))
        return sampleSDFCascade(sdf0, pos, global.sdf_origins[0].xyz, global.sdf_extents[0].xyz);
    else if (cascadeCount > 1 &&
        isInsideCascade(pos, global.sdf_origins[1].xyz, global.sdf_extents[1].xyz))
        return sampleSDFCascade(sdf1, pos, global.sdf_origins[1].xyz, global.sdf_extents[1].xyz);
    else if (cascadeCount > 2 &&
        isInsideCascade(pos, global.sdf_origins[2].xyz, global.sdf_extents[2].xyz))
        return sampleSDFCascade(sdf2, pos, global.sdf_origins[2].xyz, global.sdf_extents[2].xyz);

    return 1e10f;
}

// ============================================================================
// Fibonacci sphere direction generation + normal alignment
// ============================================================================

static float3 fibonacciDirection(uint rayIndex, uint totalRays) {
    float theta = 2.0f * PI * float(rayIndex) / GOLDEN_RATIO;
    float phi = acos(1.0f - 2.0f * (float(rayIndex) + 0.5f) / float(totalRays));
    return float3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

static float3 orientAroundNormal(float3 dir, float3 normal) {
    // Build tangent frame from normal
    float3 up = abs(normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    // Rotate direction into normal-aligned frame
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

// ============================================================================
// Sphere tracing through GlobalSDF
// ============================================================================

struct SDFHitResult {
    bool   hit;
    float3 position;
    float  distance;
};

static SDFHitResult traceSDF(float3 origin, float3 direction, float maxDist,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant ScreenProbeGlobalData& global)
{
    SDFHitResult result;
    result.hit = false;
    result.position = origin;
    result.distance = 0.0f;

    float3 pos = origin;
    float remainingDist = maxDist;

    // Use the finest cascade's voxel size as hit threshold
    float hitThreshold = global.sdf_voxel_sizes[0].x * SDF_HIT_THRESHOLD_FACTOR;

    for (uint step = 0; step < MAX_SDF_STEPS; ++step) {
        float d = sampleBestSDF(pos, sdf0, sdf1, sdf2, global);

        // Hit detected
        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = length(pos - origin);
            return result;
        }

        // Advance along ray
        float advance = max(d, hitThreshold);
        if (advance > remainingDist) break;

        pos += direction * advance;
        remainingDist -= advance;
    }

    result.distance = length(pos - origin);
    return result;
}

// ============================================================================
// Sample prev frame color at a world position
// ============================================================================

static float3 samplePrevFrameColor(float3 worldPos,
    texture2d<float, access::sample> prevColor,
    constant ScreenProbeGlobalData& global) {
    // Project to screen space
    float4 clipPos = global.view_projection * float4(worldPos, 1.0f);
    if (clipPos.w <= 0.0f) return float3(0.0f);

    float2 ndc = clipPos.xy / clipPos.w;
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - ndc.y * 0.5f - 0.5f);

    // Bounds check
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        return float3(0.0f);
    }

    sampler linearSamp(coord::normalized, filter::linear, address::clamp_to_edge);
    return prevColor.sample(linearSamp, uv).rgb;
}

// ============================================================================
// Main kernel
// ============================================================================

kernel void screen_probe_trace_rays(
    // Input SDF cascade textures
    texture3d<float, access::sample>  sdf0          [[texture(0)]],
    texture3d<float, access::sample>  sdf1          [[texture(1)]],
    texture3d<float, access::sample>  sdf2          [[texture(2)]],
    // Previous frame lit scene color
    texture2d<float, access::sample>  prevFrameColor [[texture(3)]],
    // Surface cache lighting atlas (for near-hit sampling)
    texture2d<float, access::sample>  surface_cache_lighting [[texture(4)]],

    // Input probe data buffers
    device const float4* probePositions  [[buffer(0)]],  // gridW * gridH * float4
    device const float4* probeNormals    [[buffer(1)]],  // gridW * gridH * float4

    // Output radiance buffer
    device float4*       probeRadiance   [[buffer(2)]],  // gridW * gridH * raysPerProbe * float4

    // Constant buffer
    constant ScreenProbeGlobalData& global [[buffer(3)]],

    // Surface cache card data and lookup buffers
    constant SurfaceCacheCard*        cards       [[buffer(4)]],
    constant SurfaceCacheCardLookup*  cardLookup  [[buffer(5)]],

    // Thread positioning (1D to be compatible with shared Metal compute encoder)
    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint raysPerProbe = uint(global.grid_params.w);
    uint totalRays = gridW * gridH * raysPerProbe;

    if (global_id >= totalRays) return;

    uint probeIdx = global_id / raysPerProbe;
    uint rayIdx   = global_id % raysPerProbe;

    // Read probe position and normal
    float4 posData = probePositions[probeIdx];
    float4 normData = probeNormals[probeIdx];

    // Check if probe is active (w > 0)
    if (posData.w <= 0.0f) {
        probeRadiance[global_id] = float4(0.0f, 0.0f, 0.0f, -1.0f); // -1 = inactive
        return;
    }

    float3 probePos = posData.xyz;
    float3 probeNormal = normalize(normData.xyz);

    // Generate ray direction (Fibonacci + normal alignment)
    float3 rayDir = fibonacciDirection(rayIdx, raysPerProbe);
    rayDir = orientAroundNormal(rayDir, probeNormal);

    // Offset origin slightly to avoid self-intersection
    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;

    // Trace ray through GlobalSDF
    float maxDist = global.trace_params.x;
    SDFHitResult hit = traceSDF(origin, rayDir, maxDist, sdf0, sdf1, sdf2, global);

    float3 radiance = float3(0.0f);
    float cosTheta = 0.0f;  // cosine weight for irradiance integration

    if (hit.hit) {
        // Try surface cache sampling for near hits (< 2 world units)
        bool usedSurfaceCache = false;
        float nearDistance = 2.0f;

        if (hit.distance < nearDistance && global.surface_cache_params.z > 0.5f) {
            // Iterate cards to find matching atlas UV (capped for Apple Silicon)
            uint cardCount = min(uint(global.surface_cache_params.y), 32u);
            float atlasSize = global.surface_cache_params.x;

            for (uint ci = 0; ci < cardCount && !usedSurfaceCache; ++ci) {
                float2 atlas_uv;
                if (worldToCardUV(hit.position, cards[ci], atlas_uv)) {
                    // Convert atlas pixel coords to normalized UV for sampling
                    float2 uv_norm = atlas_uv / float2(atlasSize);
                    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
                    radiance = surface_cache_lighting.sample(s, uv_norm).rgb;
                    usedSurfaceCache = true;
                }
            }
        }

        // Fall back to prev frame color if surface cache not used or not available
        if (!usedSurfaceCache) {
            radiance = samplePrevFrameColor(hit.position, prevFrameColor, global);
        }

        // NaN/Inf guard
        if (any(isnan(radiance)) || any(isinf(radiance))) {
            radiance = float3(0.0f);
        }

        // Store cosine weight (applied during averaging)
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    } else {
        // Miss ray: simple sky ambient (hemisphere-weighted sky color)
        // This prevents probes from going completely black when many rays miss
        float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
        if (skyWeight > 0.0f) {
            // Simple sky gradient: blue-ish at horizon, lighter at zenith
            float skyY = rayDir.y * 0.5f + 0.5f;
            radiance = mix(float3(0.15f, 0.18f, 0.25f), float3(0.4f, 0.5f, 0.65f), skyY);
            cosTheta = skyWeight;
        }
    }

    // Write output: xyz = radiance, w = cosine weight (>= 0 for both hit and miss with sky)
    float outputWeight = hit.hit ? cosTheta : cosTheta; // miss rays also contribute sky ambient
    probeRadiance[global_id] = float4(radiance, outputWeight);
}

// ============================================================================
// Split Trace Kernels (Apple Silicon stable)
//
// Apple Silicon: mixing texture3D reads with buffer/texture2D reads in the
// same dispatch causes flickering (same issue as DDGI TraceRays).
//
// Solution: split into two dispatches:
//   1. screen_probe_trace_sdf: texture3D-only SDF tracing → hit_distance_buffer
//   2. screen_probe_trace_finalize: buffer + texture2D, card/prev-frame lookup → probe_radiance
//
// Both use (64,1,1) threadgroups, 1D dispatch.
// ============================================================================

kernel void screen_probe_trace_sdf(
    texture3d<float, access::sample>  sdf0          [[texture(0)]],
    texture3d<float, access::sample>  sdf1          [[texture(1)]],
    texture3d<float, access::sample>  sdf2          [[texture(2)]],

    device const float4*              probePositions [[buffer(0)]],
    device const float4*              probeNormals   [[buffer(1)]],
    device float*                     hit_distance_buffer [[buffer(2)]],
    constant ScreenProbeGlobalData&   global         [[buffer(3)]],

    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint raysPerProbe = uint(global.grid_params.w);
    uint totalRays = gridW * gridH * raysPerProbe;

    if (global_id >= totalRays) return;

    uint probeIdx = global_id / raysPerProbe;
    uint rayIdx   = global_id % raysPerProbe;

    float4 posData = probePositions[probeIdx];
    if (posData.w <= 0.0f) {
        hit_distance_buffer[global_id] = -2.0f; // inactive probe
        return;
    }

    float3 probePos = posData.xyz;
    float3 probeNormal = normalize(probeNormals[probeIdx].xyz);

    float3 rayDir = fibonacciDirection(rayIdx, raysPerProbe);
    rayDir = orientAroundNormal(rayDir, probeNormal);

    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;
    float maxDist = global.trace_params.x;
    float hitThreshold = global.sdf_voxel_sizes[0].x * SDF_HIT_THRESHOLD_FACTOR;

    float3 pos = origin;
    float remainingDist = maxDist;
    float hitDistance = -1.0f; // miss

    for (uint step = 0; step < MAX_SDF_STEPS; ++step) {
        float d = sampleBestSDF(pos, sdf0, sdf1, sdf2, global);

        if (d < hitThreshold) {
            hitDistance = length(pos - origin);
            break;
        }

        float advance = max(d, hitThreshold);
        if (advance > remainingDist) break;

        pos += rayDir * advance;
        remainingDist -= advance;
    }

    hit_distance_buffer[global_id] = hitDistance;
}

kernel void screen_probe_trace_finalize(
    texture2d<float, access::sample>  prevFrameColor        [[texture(0)]],
    texture2d<float, access::sample>  surface_cache_lighting [[texture(1)]],

    device const float4*              probePositions  [[buffer(0)]],
    device const float4*              probeNormals    [[buffer(1)]],
    device const float*               hit_distance_buffer [[buffer(2)]],
    constant ScreenProbeGlobalData&   global          [[buffer(3)]],
    device float4*                    probeRadiance   [[buffer(4)]],
    constant SurfaceCacheCard*        cards           [[buffer(5)]],
    constant SurfaceCacheCardLookup*  cardLookup      [[buffer(6)]],

    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint raysPerProbe = uint(global.grid_params.w);
    uint totalRays = gridW * gridH * raysPerProbe;

    if (global_id >= totalRays) return;

    uint probeIdx = global_id / raysPerProbe;
    uint rayIdx   = global_id % raysPerProbe;

    float hitDistance = hit_distance_buffer[global_id];

    // Inactive probe
    if (hitDistance <= -1.5f) {
        probeRadiance[global_id] = float4(0.0f, 0.0f, 0.0f, -1.0f);
        return;
    }

    // Recompute ray direction for sky/weight calculation
    float3 probeNormal = normalize(probeNormals[probeIdx].xyz);
    float3 rayDir = fibonacciDirection(rayIdx, raysPerProbe);
    rayDir = orientAroundNormal(rayDir, probeNormal);

    // Miss
    if (hitDistance < 0.0f) {
        float3 radiance = float3(0.0f);
        float cosTheta = 0.0f;
        float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
        if (skyWeight > 0.0f) {
            float skyY = rayDir.y * 0.5f + 0.5f;
            radiance = mix(float3(0.15f, 0.18f, 0.25f), float3(0.4f, 0.5f, 0.65f), skyY);
            cosTheta = skyWeight;
        }
        probeRadiance[global_id] = float4(radiance, -1.0f);
        return;
    }

    // Hit: recompute position from probe data + distance
    float3 probePos = probePositions[probeIdx].xyz;
    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;
    float3 hitPos = origin + rayDir * hitDistance;

    float3 radiance = float3(0.0f);
    bool usedSurfaceCache = false;

    // Try Surface Cache for near hits
    // Apple Silicon: cap card iteration to 32 device reads to stay within limits
    if (hitDistance < 2.0f && global.surface_cache_params.z > 0.5f) {
        uint cardCount = min(uint(global.surface_cache_params.y), 32u);
        float atlasSize = global.surface_cache_params.x;

        for (uint ci = 0; ci < cardCount && !usedSurfaceCache; ++ci) {
            float2 atlas_uv;
            if (worldToCardUV(hitPos, cards[ci], atlas_uv)) {
                float2 uv_norm = atlas_uv / float2(atlasSize);
                constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
                radiance = surface_cache_lighting.sample(s, uv_norm).rgb;
                usedSurfaceCache = true;
            }
        }
    }

    // Fall back to prev frame color
    if (!usedSurfaceCache) {
        radiance = samplePrevFrameColor(hitPos, prevFrameColor, global);
    }

    // NaN/Inf guard
    if (any(isnan(radiance)) || any(isinf(radiance))) {
        radiance = float3(0.0f);
    }

    float cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    probeRadiance[global_id] = float4(radiance, cosTheta);
}
