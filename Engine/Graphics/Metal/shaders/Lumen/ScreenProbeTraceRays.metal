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
#include "SDFTraceCommon.metal"

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

    // T2.2: DDGI probe fallback — [0]=probe origin.xyz + spacing, [1]=counts.xyz + enabled(0/1)
    float4   ddgi_fb[2];

    // T5.1 hybrid trace: HZB near-field march params (near/far/mips/maxDist)
    float4   hzb_params;

    // F1 (M7): x = far_cache_start_distance — SDF rays stop there and query
    // the DDGI radiance cache at the stop point (0 disables).
    float4   cache_params;
};

// ============================================================================
// Constants
// ============================================================================

constant float PI = 3.14159265358979323846f;
constant float GOLDEN_RATIO = 1.618033988749895f;
// Was 4 — way too few for sphere tracing. Most rays failed to converge within
// 4 steps and reported "miss", producing extremely dim SPGI output.
// 32 gives rays enough iterations to actually reach geometry within maxDist.
constant uint  MAX_SDF_STEPS = 32;

// ============================================================================
// Screen Probe SDF trace wrapper
// ============================================================================

static float screenProbeSampleBestSDF(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    constant ScreenProbeGlobalData& global)
{
    return sampleBestSDF_elseIf(pos, sdf0, sdf1, sdf2,
        global.sdf_origins[0], global.sdf_origins[1], global.sdf_origins[2],
        global.sdf_extents[0], global.sdf_extents[1], global.sdf_extents[2],
        uint(global.sdf_resolutions.w));
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
// F5 (M7): per-frame jitter — byte-equivalent to EncodeLib.glsl's
// spgiJitterHash/spgiJitterRotation. Seed = camera_position.w (frame index,
// negative disables). Applied BEFORE orientAroundNormal so the whole
// Fibonacci set rotates rigidly; downstream cosine/SH weights use the actual
// rayDir and stay unbiased.
// ============================================================================

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
// EncodeLib.glsl::spgiImportanceDirection. Proposal over the rotated
// Fibonacci candidates: q ∝ max(cosθ, 0.02) × (SH-luminance + 0.05);
// inverse-CDF at (rayIdx + 0.5)/N; invNq = 1/(N·q_c) keeps the
// cosine-weighted SH projection unbiased (importance resampling).
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
// Sphere tracing through GlobalSDF
// ============================================================================
// SDFHitResult struct and sampleBestSDF_elseIf are provided by SDFTraceCommon.metal.

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

    float hitThreshold = global.sdf_voxel_sizes[0].x * 0.5f;
    float minStep = global.sdf_voxel_sizes[0].x * 0.25f;

    for (uint step = 0; step < MAX_SDF_STEPS; ++step) {
        float d = screenProbeSampleBestSDF(pos, sdf0, sdf1, sdf2, global);

        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = length(pos - origin);
            return result;
        }

        // Relaxed advancement: overstep when far, conservative when close
        float advance;
        if (d > hitThreshold * 4.0f) {
            advance = d * 1.2f;
        } else {
            advance = max(d, minStep);
        }
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

    // T1.3: uniform card grid (header + prefix sums / card indices)
    device const uint*                sc_grid_offsets [[buffer(6)]],
    device const uint*                sc_grid_cards   [[buffer(7)]],
    // T2.2: DDGI probe irradiance (SH3, 16B-stride float3 elements) for miss fallback
    device const float3*              ddgi_irradiance [[buffer(8)]],
    // F2 (M7): previous frame's final probe SH — importance proposal
    device const float4*              importanceSH    [[buffer(9)]],

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

    // Generate ray direction (F2 importance resampling, or Fibonacci +
    // jitter). invNq scales the output weight (cosθ/(N·q)).
    float3 rayDir;
    float invNq = 1.0f;
    if (global.cache_params.y > 0.5f) {
        uint hb = probeIdx * 4u;
        float4 h0 = importanceSH[hb + 0u];
        float4 h1 = importanceSH[hb + 1u];
        float4 h2 = importanceSH[hb + 2u];
        float4 h3 = importanceSH[hb + 3u];
        rayDir = spgiImportanceDirection(rayIdx, raysPerProbe, probeIdx,
                                         global.camera_position.w, probeNormal,
                                         h0.rgb, h1.rgb, h2.rgb, h3.rgb, invNq);
        rayDir = orientAroundNormal(rayDir, probeNormal);
    } else {
        rayDir = fibonacciDirection(rayIdx, raysPerProbe);
        rayDir = spgiJitterRotation(probeIdx, global.camera_position.w) * rayDir;
        rayDir = orientAroundNormal(rayDir, probeNormal);
    }

    // Offset origin slightly to avoid self-intersection
    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;

    // Trace ray through GlobalSDF. F1: cap the march at
    // far_cache_start_distance — unresolved rays query the DDGI radiance
    // cache at the stop point instead of marching the full maxDist.
    float cacheStart = global.cache_params.x;
    float maxDist = (cacheStart > 0.0f)
        ? min(global.trace_params.x, cacheStart)
        : global.trace_params.x;
    SDFHitResult hit = traceSDF(origin, rayDir, maxDist, sdf0, sdf1, sdf2, global);

    float3 radiance = float3(0.0f);
    float cosTheta = 0.0f;  // cosine weight for irradiance integration

    if (hit.hit) {
        // Try surface cache sampling for near hits (< 2 world units)
        bool usedSurfaceCache = false;
        float nearDistance = 2.0f;

        if (hit.distance < nearDistance && global.surface_cache_params.z > 0.5f) {
            // T1.3: grid lookup — candidates from the hit position's cell
            // (≤ kMaxCardsPerGridCell entries) instead of scanning 32 cards.
            float atlasSize = global.surface_cache_params.x;

            uint4 gh0 = uint4(sc_grid_offsets[0], sc_grid_offsets[1],
                              sc_grid_offsets[2], sc_grid_offsets[3]);
            uint4 gh1 = uint4(sc_grid_offsets[4], sc_grid_offsets[5],
                              sc_grid_offsets[6], sc_grid_offsets[7]);
            uint4 gh2 = uint4(sc_grid_offsets[8], sc_grid_offsets[9],
                              sc_grid_offsets[10], sc_grid_offsets[11]);
            SCGridParams gp = scGridDecode(gh0, gh1, gh2);
            uint cell = scGridCellIndex(gp, hit.position);
            if (cell != 0xFFFFFFFFu) {
                uint begin = sc_grid_offsets[16u + cell];
                uint end   = sc_grid_offsets[17u + cell];
                for (uint gi = begin; gi < end && !usedSurfaceCache; ++gi) {
                    float2 atlas_uv;
                    if (worldToCardUV(hit.position, cards[sc_grid_cards[gi]], atlas_uv)) {
                        // Convert atlas pixel coords to normalized UV for sampling
                        float2 uv_norm = atlas_uv / float2(atlasSize);
                        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
                        radiance = surface_cache_lighting.sample(s, uv_norm).rgb;
                        usedSurfaceCache = true;
                    }
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
        // Miss ray fallback chain (T2.2): DDGI probe grid → sky gradient.
        // Nearest-probe SH L0+L1 evaluated along the ray direction — the
        // Lumen-style "miss → world probe cache" edge. Sky gradient remains
        // the terminal fallback when DDGI is unavailable.
        // F1: when the far-cache early-out is active, unresolved rays query
        // the DDGI volume at the march stop point (hit.distance).
        float3 queryPos = (cacheStart > 0.0f)
            ? origin + rayDir * hit.distance
            : probePos;
        if (global.ddgi_fb[1].w > 0.5f) {
            float3 org = global.ddgi_fb[0].xyz;
            float spacing = max(global.ddgi_fb[0].w, 0.01f);
            int3 counts = int3(global.ddgi_fb[1].xyz);
            int3 gc = int3(floor((queryPos - org) / spacing));
            gc = clamp(gc, int3(0), counts - int3(1));
            uint pidx = uint(gc.x) + uint(counts.x) * (uint(gc.y) + uint(counts.y) * uint(gc.z));
            // SH3 coefficients @ 16B stride (matches LumenDDGIPass irradiance buffers)
            float3 sh0 = ddgi_irradiance[pidx * 9u + 0u];
            float3 sh1 = ddgi_irradiance[pidx * 9u + 1u];
            float3 sh2 = ddgi_irradiance[pidx * 9u + 2u];
            float3 sh3 = ddgi_irradiance[pidx * 9u + 3u];
            radiance = max(0.0f,
                0.282095f * sh0
                - 0.488603f * sh1 * rayDir.y
                + 0.488603f * sh2 * rayDir.z
                - 0.488603f * sh3 * rayDir.x);
            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
        } else {
            // Simple sky gradient: blue-ish at horizon, lighter at zenith
            float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight > 0.0f) {
                float skyY = rayDir.y * 0.5f + 0.5f;
                radiance = mix(float3(0.15f, 0.18f, 0.25f), float3(0.4f, 0.5f, 0.65f), skyY);
                cosTheta = skyWeight;
            }
        }
    }

    // Write output: xyz = radiance, w = cosine weight × importance factor (F2)
    float outputWeight = (hit.hit ? cosTheta : cosTheta) * invNq; // miss rays also contribute sky ambient
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

// ============================================================================
// T5.1 2a: HZB near-field trace — texture2D (mip chain) + probe buffers only.
// Resolved rays write (dist, hitUV.xy, 1); everything else stays unresolved.
// ============================================================================
kernel void screen_probe_trace_hzb(
    texture2d<float, access::sample>  hzb_texture    [[texture(0)]],

    device const float4*              probePositions [[buffer(0)]],
    device const float4*              probeNormals   [[buffer(1)]],
    device float4*                    hit_buffer     [[buffer(2)]],
    constant ScreenProbeGlobalData&   global         [[buffer(3)]],
    device const float4*              importanceSH   [[buffer(4)]],  // F2

    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint raysPerProbe = uint(global.grid_params.w);
    uint totalRays = gridW * gridH * raysPerProbe;
    if (global_id >= totalRays) return;

    hit_buffer[global_id] = float4(-1.0f, 0.0f, 0.0f, 0.0f);

    float4 posData = probePositions[global_id / raysPerProbe];
    if (posData.w <= 0.0f) return;

    float3 probeNormal = normalize(probeNormals[global_id / raysPerProbe].xyz);
    uint rayIdx = global_id % raysPerProbe;
    uint isIdx = global_id / raysPerProbe;   // F2 importance probe index
    float3 rayDir;
    float invNq;   // unused here; Finalize owns the importance weight
    if (global.cache_params.y > 0.5f) {
        uint hb = isIdx * 4u;
        float4 h0 = importanceSH[hb + 0u];
        float4 h1 = importanceSH[hb + 1u];
        float4 h2 = importanceSH[hb + 2u];
        float4 h3 = importanceSH[hb + 3u];
        rayDir = spgiImportanceDirection(rayIdx, raysPerProbe, isIdx,
                                         global.camera_position.w, probeNormal,
                                         h0.rgb, h1.rgb, h2.rgb, h3.rgb, invNq);
        rayDir = orientAroundNormal(rayDir, probeNormal);
    } else {
        rayDir = fibonacciDirection(rayIdx, raysPerProbe);
        rayDir = spgiJitterRotation(isIdx, global.camera_position.w) * rayDir;
        rayDir = orientAroundNormal(rayDir, probeNormal);
    }
    float3 origin = posData.xyz + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;

    float near = global.hzb_params.x;
    float far  = global.hzb_params.y;
    uint mipLevels = max(uint(global.hzb_params.z), 1u);
    float maxDist = global.hzb_params.w;
    if (maxDist <= 0.0f || global.hzb_params.z <= 0.0f) return;

    constexpr uint MAX_HZB_STEPS = 32u;
    float baseStep = maxDist / float(MAX_HZB_STEPS);
    float t = 0.0f;
    uint mip = 1u;
    float thickness = 0.3f;

    for (uint step = 0u; step < MAX_HZB_STEPS; ++step) {
        float mipScale = float(1u << mip);
        t += baseStep * mipScale;
        if (t > maxDist) break;

        float3 world = origin + rayDir * t;
        float4 clip = global.view_projection * float4(world, 1.0f);
        if (clip.w <= 0.0f) break;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = float2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) break;

        constexpr sampler s(coord::normalized, filter::nearest, address::clamp_to_edge);
        float sceneNDC = hzb_texture.sample(s, uv, level(float(mip))).r;
        // Linearize both (Vulkan [0,1] NDC convention, matches the HZB chain).
        float sceneLin = near * far / (far - sceneNDC * (far - near));
        float rayLin   = near * far / (far - ndc.z * (far - near));

        if (rayLin > sceneLin) {
            if (mip == 0u) {
                if (rayLin <= sceneLin + thickness) {
                    hit_buffer[global_id] = float4(t, uv.x, uv.y, 1.0f);
                    return;
                }
            } else {
                t -= baseStep * mipScale;
                mip--;
            }
        } else {
            mip = min(mip + 1u, mipLevels - 1u);
        }
    }
}

// ============================================================================
// T5.1 2b: SDF far-field trace — texture3D + probe buffers only. Skips rays
// the HZB kernel already resolved (hit_buffer.w == 1).
// ============================================================================
kernel void screen_probe_trace_sdf(
    texture3d<float, access::sample>  sdf0          [[texture(0)]],
    texture3d<float, access::sample>  sdf1          [[texture(1)]],
    texture3d<float, access::sample>  sdf2          [[texture(2)]],

    device const float4*              probePositions [[buffer(0)]],
    device const float4*              probeNormals   [[buffer(1)]],
    device float4*                    hit_buffer     [[buffer(2)]],
    constant ScreenProbeGlobalData&   global         [[buffer(3)]],
    device const float4*              importanceSH   [[buffer(4)]],  // F2

    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint raysPerProbe = uint(global.grid_params.w);
    uint totalRays = gridW * gridH * raysPerProbe;

    if (global_id >= totalRays) return;

    // HZB kernel already resolved this ray — keep its result.
    if (global.hzb_params.z > 0.0f && hit_buffer[global_id].w == 1.0f) return;

    uint probeIdx = global_id / raysPerProbe;
    uint rayIdx   = global_id % raysPerProbe;

    float4 posData = probePositions[probeIdx];
    if (posData.w <= 0.0f) {
        hit_buffer[global_id] = float4(-1.0f, 0.0f, 0.0f, 0.0f); // inactive probe
        return;
    }

    float3 probePos = posData.xyz;
    float3 probeNormal = normalize(probeNormals[probeIdx].xyz);

    float3 rayDir;
    float invNq;   // unused here; Finalize owns the importance weight
    if (global.cache_params.y > 0.5f) {
        // F2 (M7) 形态 c: importance resampling (see spgiImportanceDirection).
        uint hb = probeIdx * 4u;
        float4 h0 = importanceSH[hb + 0u];
        float4 h1 = importanceSH[hb + 1u];
        float4 h2 = importanceSH[hb + 2u];
        float4 h3 = importanceSH[hb + 3u];
        rayDir = spgiImportanceDirection(rayIdx, raysPerProbe, probeIdx,
                                         global.camera_position.w, probeNormal,
                                         h0.rgb, h1.rgb, h2.rgb, h3.rgb, invNq);
        rayDir = orientAroundNormal(rayDir, probeNormal);
    } else {
        rayDir = fibonacciDirection(rayIdx, raysPerProbe);
        rayDir = spgiJitterRotation(probeIdx, global.camera_position.w) * rayDir;
        rayDir = orientAroundNormal(rayDir, probeNormal);
    }

    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;
    // F1: cap the march at far_cache_start (0 disables).
    float cacheStart = global.cache_params.x;
    float maxDist = (cacheStart > 0.0f)
        ? min(global.trace_params.x, cacheStart)
        : global.trace_params.x;
    float hitThreshold = global.sdf_voxel_sizes[0].x * 0.5f;
    float minStep = global.sdf_voxel_sizes[0].x * 0.25f;

    float3 pos = origin;
    float remainingDist = maxDist;
    float hitDistance = -1.0f; // miss

    for (uint step = 0; step < MAX_SDF_STEPS; ++step) {
        float d = screenProbeSampleBestSDF(pos, sdf0, sdf1, sdf2, global);

        if (d < hitThreshold) {
            hitDistance = length(pos - origin);
            break;
        }

        // Relaxed advancement: overstep when far, conservative when close
        float advance;
        if (d > hitThreshold * 4.0f) {
            advance = d * 1.2f;
        } else {
            advance = max(d, minStep);
        }
        if (advance > remainingDist) break;

        pos += rayDir * advance;
        remainingDist -= advance;
    }

    if (hitDistance > 0.0f) {
        hit_buffer[global_id] = float4(hitDistance, 0.0f, 0.0f, 2.0f);
    } else if (cacheStart > 0.0f) {
        // F1 far-cache: x = travelled distance (stop point = origin + dir * x).
        hit_buffer[global_id] = float4(length(pos - origin), 0.0f, 0.0f, 3.0f);
    } else {
        hit_buffer[global_id] = float4(-1.0f, 0.0f, 0.0f, 0.0f);
    }
}

// ============================================================================
// T5.1 2c: radiance finalize — texture2D + buffers only (no texture3D).
//   w==1 HZB hit → prev frame color at hit UV (sharp screen-space detail)
//   w==2 SDF hit → SC card grid (T1.3) for near hits → prev color projection
//   w==3 far-cache (F1) → DDGI 4-probe tetrahedral L0+L1 at the stop point
//   w==0 miss    → DDGI probe fallback (T2.2) → sky gradient
// ============================================================================
kernel void screen_probe_trace_finalize(
    texture2d<float, access::sample>  prevFrameColor        [[texture(0)]],
    texture2d<float, access::sample>  surface_cache_lighting [[texture(1)]],

    device const float4*              probePositions  [[buffer(0)]],
    device const float4*              probeNormals    [[buffer(1)]],
    device const float4*              hit_buffer      [[buffer(2)]],
    constant ScreenProbeGlobalData&   global          [[buffer(3)]],
    device float4*                    probeRadiance   [[buffer(4)]],
    constant SurfaceCacheCard*        cards           [[buffer(5)]],
    constant SurfaceCacheCardLookup*  cardLookup      [[buffer(6)]],
    device const uint*                sc_grid_offsets [[buffer(7)]],
    device const uint*                sc_grid_cards   [[buffer(8)]],
    device const float4*              ddgiIrradiance  [[buffer(9)]],
    device const float4*              importanceSH    [[buffer(10)]],  // F2

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
        probeRadiance[global_id] = float4(0.0f, 0.0f, 0.0f, -1.0f); // inactive
        return;
    }

    float3 probeNormal = normalize(probeNormals[probeIdx].xyz);
    float3 rayDir;
    float invNq = 1.0f;   // F2 importance weight: cosθ/(N·q), 1 when disabled
    if (global.cache_params.y > 0.5f) {
        // F2 (M7) 形态 c: importance resampling (see spgiImportanceDirection).
        uint hb = probeIdx * 4u;
        float4 h0 = importanceSH[hb + 0u];
        float4 h1 = importanceSH[hb + 1u];
        float4 h2 = importanceSH[hb + 2u];
        float4 h3 = importanceSH[hb + 3u];
        rayDir = spgiImportanceDirection(rayIdx, raysPerProbe, probeIdx,
                                         global.camera_position.w, probeNormal,
                                         h0.rgb, h1.rgb, h2.rgb, h3.rgb, invNq);
        rayDir = orientAroundNormal(rayDir, probeNormal);
    } else {
        rayDir = fibonacciDirection(rayIdx, raysPerProbe);
        rayDir = spgiJitterRotation(probeIdx, global.camera_position.w) * rayDir;
        rayDir = orientAroundNormal(rayDir, probeNormal);
    }
    float3 origin = posData.xyz + probeNormal * global.sdf_voxel_sizes[0].x * 2.0f;

    float4 hit = hit_buffer[global_id];
    float3 radiance = float3(0.0f);
    float cosTheta = 0.0f;

    if (hit.w == 1.0f) {
        // HZB near-field hit — sharp screen-space radiance at the hit UV.
        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
        radiance = prevFrameColor.sample(s, hit.yz).rgb;
        if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0.0f);
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    } else if (hit.w == 2.0f) {
        // SDF far-field hit — SC card grid (T1.3) near / prev color projection.
        float3 hitPos = origin + rayDir * hit.x;
        bool usedSurfaceCache = false;

        if (hit.x < 2.0f && global.surface_cache_params.z > 0.5f) {
            float atlasSize = global.surface_cache_params.x;
            uint4 gh0 = uint4(sc_grid_offsets[0], sc_grid_offsets[1],
                              sc_grid_offsets[2], sc_grid_offsets[3]);
            uint4 gh1 = uint4(sc_grid_offsets[4], sc_grid_offsets[5],
                              sc_grid_offsets[6], sc_grid_offsets[7]);
            uint4 gh2 = uint4(sc_grid_offsets[8], sc_grid_offsets[9],
                              sc_grid_offsets[10], sc_grid_offsets[11]);
            SCGridParams gp = scGridDecode(gh0, gh1, gh2);
            uint cell = scGridCellIndex(gp, hitPos);
            if (cell != 0xFFFFFFFFu) {
                uint begin = sc_grid_offsets[16u + cell];
                uint end   = sc_grid_offsets[17u + cell];
                uint reads = 0u;
                for (uint gi = begin; gi < end && reads < 8u; ++gi, ++reads) {
                    float2 atlas_uv;
                    if (worldToCardUV(hitPos, cards[sc_grid_cards[gi]], atlas_uv)) {
                        float2 uv_norm = atlas_uv / float2(atlasSize);
                        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
                        radiance = surface_cache_lighting.sample(s, uv_norm).rgb;
                        usedSurfaceCache = true;
                        break;
                    }
                }
            }
        }

        if (!usedSurfaceCache) {
            radiance = samplePrevFrameColor(hitPos, prevFrameColor, global);
        }
        if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0.0f);
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    } else if (hit.w == 3.0f) {
        // F1 (M7) far-cache: the SDF march stopped at far_cache_start —
        // query the DDGI radiance cache at the stop point (UE world-probe
        // semantics). Read budget: 4-probe tetrahedral × L0+L1 = 16 buffer
        // reads. Mirrors ScreenProbeTraceFinalize.comp byte-for-byte.
        float3 stopPos = origin + rayDir * max(hit.x, 0.0f);
        bool ddgiOk = false;
        if (global.ddgi_fb[1].w > 0.5f) {
            float3 org = global.ddgi_fb[0].xyz;
            float spacing = max(global.ddgi_fb[0].w, 0.01f);
            int3 counts = int3(global.ddgi_fb[1].xyz);
            float3 gp = (stopPos - org) / spacing;
            float3 gridMax = float3(counts - int3(1));
            if (all(gp >= float3(0.0f)) && all(gp <= gridMax)) {
                float3 f = clamp(fract(gp), float3(0.0f), float3(1.0f));
                int3 b0 = clamp(int3(floor(gp)), int3(0), counts - int3(1));

                // Sort the three axes by descending fractional part.
                int ax = (f.x >= f.y && f.x >= f.z) ? 0 : ((f.y >= f.z) ? 1 : 2);
                int ay;
                if (ax == 0)      ay = (f.y >= f.z) ? 1 : 2;
                else if (ax == 1) ay = (f.x >= f.z) ? 0 : 2;
                else              ay = (f.x >= f.y) ? 0 : 1;
                int az = 3 - ax - ay;
                float fa = f[ax], fb = f[ay], fc = f[az];

                int3 c0 = b0;
                int3 c1 = min(b0 + select(int3(0), int3(1), int3(ax) == int3(0, 1, 2)), counts - int3(1));
                int3 c2 = min(c1 + select(int3(0), int3(1), int3(ay) == int3(0, 1, 2)), counts - int3(1));
                int3 c3 = min(c2 + select(int3(0), int3(1), int3(az) == int3(0, 1, 2)), counts - int3(1));
                float w0 = 1.0f - fa, w1 = fa - fb, w2 = fb - fc, w3 = fc;

                int3 cn[4] = { c0, c1, c2, c3 };
                float wn[4] = { w0, w1, w2, w3 };

                float3 acc = float3(0.0f);
                float wsum = 0.0f;
                for (int i = 0; i < 4; ++i) {
                    if (wn[i] <= 0.0f) continue;
                    uint pidx = uint(cn[i].x)
                              + uint(counts.x) * (uint(cn[i].y) + uint(counts.y) * uint(cn[i].z));
                    // L0 + L1 only (4 reads), canonical DDGI basis.
                    float3 sh0 = ddgiIrradiance[pidx * 9u + 0u].xyz;
                    float3 sh1 = ddgiIrradiance[pidx * 9u + 1u].xyz;
                    float3 sh2 = ddgiIrradiance[pidx * 9u + 2u].xyz;
                    float3 sh3 = ddgiIrradiance[pidx * 9u + 3u].xyz;
                    float3 irr = 0.282095f * sh0
                               - 0.488603f * sh1 * rayDir.y
                               + 0.488603f * sh2 * rayDir.z
                               - 0.488603f * sh3 * rayDir.x;
                    acc += max(irr, float3(0.0f)) * wn[i];
                    wsum += wn[i];
                }
                if (wsum > 0.0f) {
                    radiance = acc / wsum;
                    cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                    ddgiOk = true;
                }
            }
        }
        if (!ddgiOk) {
            // DDGI unavailable / stop point outside the volume → sky gradient.
            float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight > 0.0f) {
                float skyY = rayDir.y * 0.5f + 0.5f;
                radiance = mix(float3(0.15f, 0.18f, 0.25f), float3(0.4f, 0.5f, 0.65f), skyY);
                cosTheta = skyWeight;
            }
        }
    } else {
        // Miss → DDGI probe fallback (T2.2) → sky gradient.
        if (global.ddgi_fb[1].w > 0.5f) {
            float3 org = global.ddgi_fb[0].xyz;
            float spacing = max(global.ddgi_fb[0].w, 0.01f);
            int3 counts = int3(global.ddgi_fb[1].xyz);
            int3 gc = int3(floor((posData.xyz - org) / spacing));
            gc = clamp(gc, int3(0), counts - int3(1));
            uint pidx = uint(gc.x) + uint(counts.x) * (uint(gc.y) + uint(counts.y) * uint(gc.z));
            // SH3 coefficients @ 16B stride (vec4 view of the DDGI buffer)
            float3 sh0 = ddgiIrradiance[pidx * 9u + 0u].xyz;
            float3 sh1 = ddgiIrradiance[pidx * 9u + 1u].xyz;
            float3 sh2 = ddgiIrradiance[pidx * 9u + 2u].xyz;
            float3 sh3 = ddgiIrradiance[pidx * 9u + 3u].xyz;
            radiance = max(float3(0.0f),
                0.282095f * sh0
                - 0.488603f * sh1 * rayDir.y
                + 0.488603f * sh2 * rayDir.z
                - 0.488603f * sh3 * rayDir.x);
            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
        } else {
            float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight > 0.0f) {
                float skyY = rayDir.y * 0.5f + 0.5f;
                radiance = mix(float3(0.15f, 0.18f, 0.25f), float3(0.4f, 0.5f, 0.65f), skyY);
                cosTheta = skyWeight;
            }
        }
    }

    // w carries the SH-projection weight: cosθ × importance factor (F2).
    probeRadiance[global_id] = float4(radiance, cosTheta * invNq);
}
