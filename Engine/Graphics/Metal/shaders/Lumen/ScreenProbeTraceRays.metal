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
    // CPU tail: diagnostics are consumed by finalize; keep all following
    // offsets explicit so future readers cannot silently truncate the ABI.
    float4   diag_params;
    float4   hint_params;
    float4   sky_params;
    float4   wrc_fb[2];
    float4   repr_params;
    float4   tier_params;
    float4   tier_counts;
};
static_assert(sizeof(ScreenProbeGlobalData) == 544, "Screen probe CPU/MSL ABI");
static_assert(__builtin_offsetof(ScreenProbeGlobalData, diag_params) == 416, "SPGI diagnostics ABI");

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
                   + sh1 * (-0.488603f * world.y)
                   + sh2 * (0.488603f * world.z)
                   + sh3 * (-0.488603f * world.x);
        float lum = max(dot(irr, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
        pdf[i] = max(local.z, 0.02f) * (lum + 0.05f);
        sum += pdf[i];
    }

    // Defensive mixture keeps every candidate reachable even when history
    // misses a bright lobe. q >= 0.25/N bounds invNq <= 4 without clipping
    // radiance or biasing the estimator; PDF correction uses this mixture.
    for (uint i = 0u; i < n; ++i)
        pdf[i] = 0.25f / float(n) + 0.75f * pdf[i] / max(sum, 1e-6f);
    sum = 1.0f;

    // Jitter within each CDF stratum. A fixed midpoint can permanently omit
    // small-probability candidates; the PDF correction needs random coverage.
    uint sampleSeed = frameSeed < 0.0f ? 0u : uint(frameSeed);
    uint sampleHash = spgiJitterHash(probeIdx * 0x9e3779b9u ^ sampleSeed ^ (rayIdx * 0x85ebca6bu));
    float jitter = (float(sampleHash & 0x00ffffffu) + 0.5f) / 16777216.0f;
    float u = (float(rayIdx) + jitter) / float(n) * sum;
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
    // T2.2: DDGI probe irradiance (SH3, 16B-stride float4 elements) for miss
    // fallback — A8: must match finalize's float4 view of the same buffer.
    device const float4*              ddgi_irradiance [[buffer(8)]],
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
                        // C4: half-texel inset keeps bilinear sampling inside
                        // this card's rect (shelf-packed atlas bleeds otherwise).
                        const SurfaceCacheCard insetCard = cards[sc_grid_cards[gi]];
                        atlas_uv = clamp(atlas_uv,
                            float2(insetCard.atlas_offset_x, insetCard.atlas_offset_y) + 0.5,
                            float2(insetCard.atlas_offset_x + insetCard.resolution,
                                   insetCard.atlas_offset_y + insetCard.resolution) - 0.5);
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
            float3 sh0 = ddgi_irradiance[pidx * 9u + 0u].xyz;
            float3 sh1 = ddgi_irradiance[pidx * 9u + 1u].xyz;
            float3 sh2 = ddgi_irradiance[pidx * 9u + 2u].xyz;
            float3 sh3 = ddgi_irradiance[pidx * 9u + 3u].xyz;
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
    // B5: sign encodes geometry hit (positive) vs miss→sky (negative).
    // Average integrates |w| but counts hits with w > 0 — the old
    // all-positive encoding made hitRatio ≡ 1 and killed confidence.
    float outputWeight = (hit.hit ? cosTheta : -cosTheta) * invNq;
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

    // Match frozen Vulkan: grazing paths and camera-plane crossings defer
    // to the world trace instead of borrowing a near-field screen sample.
    const float4 clip0=global.view_projection*float4(origin,1.0f);
    const float4 clip1=global.view_projection*float4(origin+rayDir*maxDist,1.0f);
    if(clip0.w<=0.0f || clip1.w<=0.0f)return;
    const float2 uv0=clip0.xy/clip0.w*0.5f+0.5f;
    const float2 uv1=clip1.xy/clip1.w*0.5f+0.5f;
    if(distance(uv0,uv1)>1.5f)return;

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

        // Explicit texel coordinates preserve Vulkan texelFetch boundaries;
        // normalized sampler precision can select a neighboring HZB cell.
        const uint2 mipSize(hzb_texture.get_width(mip),hzb_texture.get_height(mip));
        const uint2 mipCoord=min(uint2(uv*float2(mipSize)),mipSize-1);
        float sceneNDC = hzb_texture.read(mipCoord,mip).r;
        // Linearize both (Vulkan [0,1] NDC convention, matches the HZB chain).
        float sceneLin = near * far / (far - sceneNDC * (far - near));
        float rayLin   = near * far / (far - ndc.z * (far - near));

        if (rayLin > sceneLin) {
            if (mip == 0u) {
                if (rayLin <= sceneLin + thickness) {
                    // A depth-thickness overlap is only a candidate. The
                    // captured surface must also lie within the ray's pixel
                    // footprint, otherwise it borrows light from a neighbor.
                    float2 dims = float2(hzb_texture.get_width(), hzb_texture.get_height());
                    float2 center = (floor(uv * dims) + 0.5f) / dims;
                    float2 screenNDC = float2(center.x * 2.0f - 1.0f, 1.0f - center.y * 2.0f);
                    float4 hp = global.inv_view_projection * float4(screenNDC, sceneNDC, 1.0f);
                    if (abs(hp.w) < 1e-6f) continue;
                    float3 surface = hp.xyz / hp.w;
                    float4 px = global.inv_view_projection * float4(screenNDC + float2(2.0f/dims.x, 0.0f), sceneNDC, 1.0f);
                    if (abs(px.w) < 1e-6f) continue;
                    float footprint = distance(surface, px.xyz / px.w);
                    float alongRay = dot(surface - origin, rayDir);
                    float rayError = distance(surface, origin + rayDir * alongRay);
                    if (alongRay <= 0.0f || alongRay > maxDist || rayError > max(0.02f, footprint * 1.5f)) continue;
                    hit_buffer[global_id] = float4(alongRay, center.x, center.y, 1.0f);
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

    // T4.3 二批: per-mesh detail SDF slots (dummy-cascade-bound when off)
    texture3d<float, access::sample>  detail0       [[texture(3)]],
    texture3d<float, access::sample>  detail1       [[texture(4)]],
    texture3d<float, access::sample>  detail2       [[texture(5)]],
    texture3d<float, access::sample>  detail3       [[texture(6)]],

    device const float4*              probePositions [[buffer(0)]],
    device const float4*              probeNormals   [[buffer(1)]],
    device float4*                    hit_buffer     [[buffer(2)]],
    constant ScreenProbeGlobalData&   global         [[buffer(3)]],
    device const float4*              importanceSH   [[buffer(4)]],  // F2
    constant DetailSDFTableGPU&       detail_table   [[buffer(5)]],  // T4.3 二批

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

    // The SDF distance is relative to this origin; Finalize uses the same
    // candidate bias. HZB keeps its independent two-voxel origin.
    const bool measured = global.tier_counts.w > 0.5f;
    float3 origin = probePos + probeNormal * global.sdf_voxel_sizes[0].x * (measured ? 0.25f : 2.0f);
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
            advance = d * (measured ? global.hint_params.z : 1.2f);
        } else {
            advance = max(d, minStep);
        }
        if (advance > remainingDist) break;

        pos += rayDir * advance;
        remainingDist -= advance;
    }

    if (measured ? hitDistance >= 0.0f : hitDistance > 0.0f) {
        // T4.3 二批: per-mesh detail refinement of the coarse GlobalSDF hit
        // (distance only — screen probes carry their own GBuffer normals).
        float3 detailN;
        if (detailSDFRefine(detail_table, detail0, detail1, detail2, detail3,
                            origin, rayDir, hitDistance, detailN)) {
            // refined hitDistance already in out-param
        }
        float packedHint = 0.0f;
        if (measured) {
            const float3 hitPos = origin + rayDir * hitDistance;
            const float h = max(global.sdf_voxel_sizes[0].x, 0.0001f);
            float3 gradient;
            float3 smoothGradient;
            for (uint axis=0;axis<3;++axis) {
                float3 offset=float3(0.0f);offset[axis]=h;
                gradient[axis]=screenProbeSampleBestSDF(hitPos+offset,sdf0,sdf1,sdf2,global)
                    -screenProbeSampleBestSDF(hitPos-offset,sdf0,sdf1,sdf2,global);
                // The frozen optional smooth hint samples cascade zero only.
                if (global.hint_params.x>0.5f)
                    smoothGradient[axis]=sampleSDFCascade(sdf0,hitPos+offset,global.sdf_origins[0].xyz,global.sdf_extents[0].xyz)
                        -sampleSDFCascade(sdf0,hitPos-offset,global.sdf_origins[0].xyz,global.sdf_extents[0].xyz);
            }
            float3 normalHint=normalize(gradient+float3(0.000001f));
            if (global.hint_params.x>0.5f)
                normalHint=normalize(mix(normalHint,normalize(smoothGradient+float3(0.000001f)),0.75f));
            // GLSL packSnorm2x16 layout, carried as bits rather than a float.
            const short2 encoded=short2(round(clamp(octEncode(normalHint)*2.0f-1.0f,float2(-1.0f),float2(1.0f))*32767.0f));
            const ushort2 bits=as_type<ushort2>(encoded);
            packedHint=as_type<float>(uint(bits.x)|(uint(bits.y)<<16u));
        }
        hit_buffer[global_id] = float4(hitDistance, packedHint, 0.0f, 2.0f);
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
//   w==3 far-cache (F1) → T3.5 radiance cascade SH-L1 directional evaluation
//        (legacy: DDGI 4-probe tetrahedral L0+L1 at the stop point)
//   w==0 miss    → DDGI probe fallback (T2.2) → sky gradient
// T3.5 note: textures (2)..(5) add FOUR texture3d texel fetches (unfiltered)
// to this kernel — within the Apple Silicon read budget, monitored via
// PRIMAL_GI_CASCADE=0 A/B.
// ============================================================================
// Native counterpart of the frozen reference's world-grid lookup with SDF
// depth-history weighting. tier_counts.w selects the measured producer contract;
// unknown depth is excluded and attenuation survives normalization.
static bool spgiWorldRadiance(float3 receiver, float3 receiverNormal, float3 direction,
                             constant ScreenProbeGlobalData& global,
                             device const float4* sh, device const float* depthHistory,
                             device const float4* effectiveOrigins, thread float3& radiance) {
    if (global.ddgi_fb[1].w <= 0.5f) return false;
    const int3 counts = int3(global.ddgi_fb[1].xyz);
    if (any(counts <= int3(0))) return false;
    const float spacing = max(global.ddgi_fb[0].w, 0.01f);
    const float3 origin = global.ddgi_fb[0].xyz;
    const float3 gp = (receiver - origin) / spacing;
    if (!all(isfinite(gp)) || any(gp < float3(0.0f)) || any(gp > float3(counts - 1))) return false;
    const int3 low = clamp(int3(floor(gp)), int3(0), counts - 1);
    const int3 high = min(low + 1, counts - 1);
    const float3 fraction = clamp(fract(gp), float3(0.0f), float3(1.0f));
    const float dx = direction.x, dy = direction.y, dz = direction.z;
    const float basis[9] = {0.282095f, -0.488603f * dy, 0.488603f * dz,
        -0.488603f * dx, 1.092548f * dy * dx, -1.092548f * dy * dz,
        0.315392f * (3.0f * dz * dz - 1.0f), -1.092548f * dx * dz,
        0.546274f * (dx * dx - dy * dy)};
    float3 sum = float3(0.0f);
    float weightSum = 0.0f;
    float unoccludedWeightSum = 0.0f;
    const bool measured = global.tier_counts.w > 0.5f;
    const bool useVisibility = measured || global.hint_params.w > 0.5f;
    for (uint corner = 0; corner < 8; ++corner) {
        const bool3 upper = bool3(corner & 1u, corner & 2u, corner & 4u);
        const float3 weights = select(1.0f - fraction, fraction, upper);
        const float trilinear = weights.x * weights.y * weights.z;
        if (trilinear <= 0.0f) continue;
        const int3 cell = select(low, high, upper);
        const float3 delta = origin + float3(cell) * spacing - receiver;
        const float lengthToProbe = length(delta);
        // A receiver exactly on a probe has no preferred direction.
        const float3 toProbe = lengthToProbe > 0.000001f ? delta / lengthToProbe : float3(0.0f);
        float weight = trilinear * (0.25f + 0.75f * clamp(dot(toProbe, direction) * 0.5f + 0.5f, 0.0f, 1.0f));
        unoccludedWeightSum += weight;
        const uint index = uint(cell.x + counts.x * (cell.y + counts.y * cell.z));
        if (useVisibility) {
            // The second plane stores variance. Measured mode has no variance
            // floor or minimum visibility; unclassified support contributes zero.
            const float4 relocated = effectiveOrigins[index];
            const bool measuredOrigin = relocated.w > 0.25f && all(isfinite(relocated));
            const float3 anchor = (measured ? measuredOrigin : relocated.w > 0.0f)
                ? relocated.xyz : origin + float3(cell) * spacing;
            const float3 probeToReceiver = receiver - anchor;
            const float distanceToProbe = length(probeToReceiver);
            const float3 sampleDirection = measured && !(distanceToProbe > 0.0001f)
                ? receiverNormal : probeToReceiver / max(distanceToProbe, 0.0001f);
            float2 oct = sampleDirection.xy / max(dot(abs(sampleDirection), float3(1.0f)), 0.0001f);
            if (sampleDirection.z < 0.0f)
                oct = (1.0f - abs(oct.yx)) * select(float2(-1.0f), float2(1.0f), oct >= float2(0.0f));
            const uint2 texel = uint2(clamp((oct * 0.5f + 0.5f) * 8.0f, float2(0.0f), float2(7.0f)));
            const uint depthIndex = index * 128u + texel.y * 8u + texel.x;
            const float mean = depthHistory[depthIndex];
            const float storedVariance = depthHistory[depthIndex + 64u];
            const float variance = measured ? storedVariance : max(storedVariance, 0.0001f);
            const float beyond = max(distanceToProbe - mean, 0.0f);
            float visibility;
            if (measured) {
                const bool known = measuredOrigin && isfinite(distanceToProbe) &&
                    isfinite(mean) && mean >= 0.0f && isfinite(variance) && variance >= 0.0f;
                visibility = known ? (beyond == 0.0f ? 1.0f : variance / (variance + beyond * beyond)) : 0.0f;
            } else {
                visibility = clamp(2.0f * variance / (variance + beyond * beyond), 0.2f, 1.0f);
            }
            visibility *= relocated.w > 0.75f ? 1.0f : (relocated.w > 0.0f ? 0.5f : 0.0f);
            // Reject corrupt support before it can poison the weighted sum.
            if (!all(isfinite(relocated)) || !isfinite(mean) || !isfinite(storedVariance)) visibility = 0.0f;
            weight *= visibility;
        }
        float3 value = float3(0.0f);
        for (uint coefficient = 0; coefficient < 9; ++coefficient)
            value += sh[index * 9u + coefficient].rgb * basis[coefficient];
        if (measured && !all(isfinite(value))) continue;
        sum += max(value, float3(0.0f)) * weight;
        weightSum += weight;
    }
    if (!(weightSum > 0.0f) && (measured || !useVisibility)) return false;
    radiance = sum / max(weightSum, 0.0001f);
    // Mode 1 selects visible neighbours. Mode 2 additionally preserves the
    // attenuation that normalization would otherwise erase.
    if (measured || global.hint_params.w > 1.5f)
        radiance *= clamp(weightSum / max(unoccludedWeightSum, 0.0001f), 0.0f, 1.0f);
    if (!all(isfinite(radiance))) radiance = float3(0.0f);
    radiance = min(radiance, float3(64.0f));
    return true;
}

static float3 spgiSkyRadiance(float3 direction, constant ScreenProbeGlobalData& global) {
    const float up = clamp(direction.y, 0.0f, 1.0f);
    // Frozen ShaderLib/SkyAtmosphere.glsl EvaluateSkyBase, without sun halo.
    return mix(float3(4.40f, 4.20f, 3.80f), float3(5.50f, 5.76f, 6.00f), pow(up, 0.45f))
        * global.sky_params.xyz;
}

kernel void screen_probe_trace_finalize(
    texture2d<float, access::sample>  prevFrameColor        [[texture(0)]],
    texture2d<float, access::sample>  surface_cache_lighting [[texture(1)]],
    texture3d<float, access::read>    radianceCascadeL0      [[texture(2)]],  // T3.5 SH-L1
    texture3d<float, access::read>    radianceCascadeL1x     [[texture(3)]],  // T3.5 SH-L1
    texture3d<float, access::read>    radianceCascadeL1y     [[texture(4)]],  // T3.5 SH-L1
    texture3d<float, access::read>    radianceCascadeL1z     [[texture(5)]],  // T3.5 SH-L1
    texture2d<float, access::read>    surface_cache_depth    [[texture(6)]],
    texture2d<float, access::read>    surface_cache_albedo   [[texture(7)]],

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
    device const float*               ddgiDepth       [[buffer(11)]],
    device const float4*              ddgiOrigins     [[buffer(12)]],

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
    float3 origin = posData.xyz + probeNormal * global.sdf_voxel_sizes[0].x
        * (global.tier_counts.w > 0.5f ? 0.25f : 2.0f);

    float4 hit = hit_buffer[global_id];
    const bool measured = global.tier_counts.w > 0.5f;
    if (measured && hit.w == 3.0f) hit = float4(-1.0f,0.0f,0.0f,0.0f);
    float3 radiance = float3(0.0f);
    float cosTheta = 0.0f;
    bool useWorldFallback = false;

    if (hit.w == 1.0f) {
        // HZB near-field hit — sharp screen-space radiance at the hit UV.
        constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
        radiance = prevFrameColor.sample(s, hit.yz).rgb;
        if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0.0f);
        // Same HDR-hit firefly knee as frozen ScreenProbeTraceFinalize.comp.
        // Below the CPU-provided knee energy passes unchanged; diagnostic 2
        // keeps the raw HZB sample for source-isolation comparisons.
        if (global.diag_params.x < 1.5f || global.diag_params.x > 2.5f) {
            float hitLum = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            radiance *= 1.0f / (1.0f + max(hitLum - global.tier_counts.y, 0.0f) * 2.0f);
        }
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    } else if (hit.w == 2.0f) {
        // SDF far-field hit — SC card grid (T1.3) near / prev color projection.
        float3 hitPos = origin + rayDir * hit.x;
        bool usedSurfaceCache = false;
        float3 hintForQuery=float3(0.0f);
        if (measured && global.diag_params.y>=0.0f) {
            const uint packed=as_type<uint>(hit.y);
            const short2 encoded=as_type<short2>(ushort2(ushort(packed&65535u),ushort(packed>>16u)));
            const float2 octS=max(float2(encoded)/32767.0f,float2(-1.0f));
            const float3 decoded=octDecode(clamp(octS*0.5f+0.5f,float2(0.0f),float2(1.0f)));
            if (dot(decoded,decoded)>0.25f)
                hintForQuery=decoded*(0.5f/max(global.diag_params.y,0.001f));
        }

        // A card projection only proves UV coverage, not surface identity.
        // Validate against captured geometry and choose the closest matching
        // surface, rather than the first card in the spatial bucket.
        if (global.surface_cache_params.z > 0.5f && (surface_cache_depth.get_width() == uint(global.surface_cache_params.x))) {
            uint4 gh0 = uint4(sc_grid_offsets[0], sc_grid_offsets[1], sc_grid_offsets[2], sc_grid_offsets[3]);
            uint4 gh1 = uint4(sc_grid_offsets[4], sc_grid_offsets[5], sc_grid_offsets[6], sc_grid_offsets[7]);
            uint4 gh2 = uint4(sc_grid_offsets[8], sc_grid_offsets[9], sc_grid_offsets[10], sc_grid_offsets[11]);
            SCGridParams gp = scGridDecode(gh0, gh1, gh2);
            uint cell = scGridCellIndex(gp, hitPos);
            // C (plan §三.C): unified query contract (shared helper, Vulkan
            // twin ScreenProbeTraceFinalize.comp) — adds the side-orientation
            // test (captured face must front the incoming ray; the old
            // texel-error loop could flip between the two cards of a
            // two-sided wall) and decomposed along/perp tolerances to the
            // existing depth + min-error scheme. Valid black stays accepted.
            float bestKey = 1e30f;
            if (cell != 0xFFFFFFFFu) {
                uint begin = sc_grid_offsets[16u + cell];
                uint end = sc_grid_offsets[17u + cell];
                for (uint gi = begin; gi < end; ++gi) {
                    uint ci = sc_grid_cards[gi];
                    float3 candidate; float key;
                    uint reject;
                    if (scQueryScreenProbeCandidate(cards[ci], hitPos, rayDir,
                            max(0.15f, global.sdf_voxel_sizes[0].x * 1.25f),
                            global.tier_counts.z > 0.5f,
                            surface_cache_depth, surface_cache_lighting, surface_cache_albedo,
                            hintForQuery, candidate, key, reject)) {
                        if (key < bestKey) {
                            bestKey = key;
                            radiance = candidate;
                            usedSurfaceCache = true;
                        }
                    }
                }
            }
        }
        if (!usedSurfaceCache) {
            // A coarse SDF hit has no matching raster-depth proof. Projecting
            // it into screen color can borrow sunlight from an occluder.
            // Use the existing world-probe estimate until a validated screen
            // hit (HZB) or a surface-cache card supplies this ray.
            useWorldFallback = true;
        }
        // SC and HZB share the same sampled-radiance policy. The existing
        // CPU diagnostic flag explicitly restores unbounded SC samples.
        if (global.diag_params.w < 0.5f) {
            float hitLum = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            radiance *= 1.0f / (1.0f + max(hitLum - global.tier_counts.y, 0.0f) * 2.0f);
        }
        if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0.0f);
        cosTheta = max(dot(probeNormal, rayDir), 0.0f);
    } else if (hit.w == 3.0f) {
        // F1 (M7) far-cache: the SDF march stopped at far_cache_start —
        // query the radiance cache at the stop point (UE world-probe
        // semantics). T3.5 + SH-L1: the 32³ cascade serves the query with
        // FOUR texel reads evaluated along rayDir (cache_params.w gates the
        // fast path); the legacy 4-probe tetrahedral × L0+L1 = 16 buffer
        // reads stays as fallback.
        // Mirrors ScreenProbeTraceFinalize.comp byte-for-byte.
        float3 stopPos = origin + rayDir * max(hit.x, 0.0f);
        bool ddgiOk = false;
        if (global.cache_params.w > 0.5f && global.ddgi_fb[1].w > 0.5f) {
            // Extent derivation — must match DispatchCascadeUpdate:
            // extent = max(counts)×spacing×2, origin = ddgi_origin − extent×0.25.
            float ddgiSpan = max(max(global.ddgi_fb[1].x, global.ddgi_fb[1].y),
                                 global.ddgi_fb[1].z) * global.ddgi_fb[0].w;
            float cExtent = max(ddgiSpan * 2.0f, 1.0f);
            float3 cOrg = global.ddgi_fb[0].xyz - float3(cExtent * 0.25f);
            float cSpacing = cExtent / 32.0f;
            float3 cgp = (stopPos - cOrg) / cSpacing;
            if (all(cgp >= float3(0.0f)) && all(cgp < float3(32.0f))) {
                // texture3d access::read does not clamp — guard manually.
                uint3 ct = min(uint3(floor(cgp)), uint3(31u));
                float4 cL0  = radianceCascadeL0.read(ct);
                float3 cL1x = radianceCascadeL1x.read(ct).rgb;
                float3 cL1y = radianceCascadeL1y.read(ct).rgb;
                float3 cL1z = radianceCascadeL1z.read(ct).rgb;
                // Validity: A = written-by-cascade, L0 nonzero (a disabled
                // cascade writes zeros). L1 coefficients are legitimately
                // negative — never gate on their sign.
                if (cL0.a > 0.5f && any(cL0.rgb > float3(0.0f))) {
                    // Canonical DDGI L0+L1 basis, same as the fallback below.
                    float3 irr = 0.282095f * cL0.rgb
                               - 0.488603f * cL1x * rayDir.y
                               + 0.488603f * cL1y * rayDir.z
                               - 0.488603f * cL1z * rayDir.x;
                    radiance = max(irr, float3(0.0f));
                    cosTheta = max(dot(probeNormal, rayDir), 0.0f);
                    ddgiOk = true;
                }
            }
        }
        if (!ddgiOk && global.ddgi_fb[1].w > 0.5f) {
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
                    // L0 + L1 only (4 reads), canonical DDGI basis. KNOWN
                    // DOWNGRADE (deliberate): DDGI stores full SH L2 (9
                    // coefficients); this fallback truncates to L1 to keep
                    // 4 reads/probe. The T3.5 cascade now carries L1 too, so
                    // no link in the far-field chain loses direction below
                    // L1; reading coeffs 4..8 here is a possible future
                    // quality step (5 extra buffer reads/ray).
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
                radiance = spgiSkyRadiance(rayDir, global);
                cosTheta = skyWeight;
            }
        }
    }
    if (useWorldFallback || (hit.w != 1.0f && hit.w != 2.0f && hit.w != 3.0f)) {
        // Full SH9 interpolation replaces nearest-probe L0+L1 snapping.
        // Outside the grid is unavailable; do not clamp onto an unrelated edge probe.
        if (spgiWorldRadiance(posData.xyz, probeNormal, rayDir, global, ddgiIrradiance, ddgiDepth, ddgiOrigins, radiance)) {
            cosTheta = max(dot(probeNormal, rayDir), 0.0f);
        } else {
            if (measured) {
                // No classified support is unknown, not a measured black sample.
                // Average excludes w=0; do not revive it through sky or WRC.
                probeRadiance[global_id] = float4(0.0f);
                return;
            }
            const float skyWeight = max(dot(probeNormal, rayDir), 0.0f);
            if (skyWeight > 0.0f) {
                radiance = spgiSkyRadiance(rayDir, global);
                cosTheta = skyWeight;
            }
        }
    }

    // w carries the SH-projection weight: cosθ × importance factor (F2).
    // B5: sign tracks measured transport, not merely an SDF stop.  Rejected
    // SC candidates and far-cache values are borrowed world radiance.
    const bool geoHit = (hit.w == 1.0f) || (hit.w == 2.0f && !useWorldFallback);
    // [E0-I R4 2026-09-21 Debug A/B/C 分通路隔离]（Vulkan 孪生
    // ScreenProbeTraceFinalize.comp 同语义）：mode 4 = fill-only（HZB/SC 置零，
    // 保留 far-cache/miss 天空链）；mode 5 = transport-only（far-cache/miss
    // 置零，保留 HZB/SC）。Metal 侧无分支统计（diag stats 为 Vulkan 通道），
    // 分支号就地派生。SPGI_TRACE_DIAG=4/5 经 diag_params.x 直通。
    {
        uint statBranch = 3u;
        if (hit.w == 1.0f) statBranch = 0u;
        else if (hit.w == 2.0f) statBranch = useWorldFallback ? 3u : 1u;
        else if (hit.w == 3.0f) statBranch = 2u;
        const float diagMode = global.diag_params.x;
        if (diagMode > 3.5f && diagMode < 4.5f) {
            if (statBranch <= 1u) radiance = float3(0.0f);
        } else if (diagMode > 4.5f && diagMode < 5.5f) {
            if (statBranch >= 2u) radiance = float3(0.0f);
        }
    }
    probeRadiance[global_id] = float4(radiance, geoHit ? cosTheta * invNq : -cosTheta * invNq);
}
