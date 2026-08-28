/**
 * @file ScreenProbePlace.metal
 * @brief Screen Probe GI - Probe placement in screen space
 *
 * Places probes on a uniform screen-space grid. Each probe samples the
 * GBuffer depth and normal at its center to determine its world position.
 * Sky pixels (depth = 0 or very far) are marked inactive.
 *
 * Dispatch: (gridW * gridH + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 *
 * Uses 1D dispatch to avoid Metal validation errors when sharing a compute
 * encoder with other passes that use scalar thread_position_in_grid.
 */

#include <metal_stdlib>
using namespace metal;

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

    // F5: w = jitter_max_world (world-space jitter clamp); xyz unused here.
    float4   surface_cache_params;

    // Offset-parity fields (must match the C++ ScreenProbeGlobalData order).
    float4   ddgi_fb[2];
    float4   hzb_params;
    // F4 (M7): z = adaptive_pos_error threshold (0 → legacy variance test).
    float4   cache_params;
};

// ============================================================================
// Helper: reconstruct world position from depth + invViewProj
// ============================================================================

static float3 reconstructWorldPos(float2 uv, float depth,
                                  constant float4x4& invVP,
                                  float2 screenDims) {
    // NDC coordinates [-1, 1]
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Clip-space position
    float4 clipPos = float4(ndc, depth, 1.0f);

    // World-space position
    float4 worldPos = invVP * clipPos;
    return worldPos.xyz / worldPos.w;
}

// F5 (M7): per-frame jitter — byte-equivalent to EncodeLib.glsl's
// spgiJitterHash/spgiJitterOffset01. Seed = camera_position.w (frame index,
// negative disables); amplitude clamp = surface_cache_params.w.
static uint spgiJitterHash(uint v) {
    v ^= v >> 16u; v *= 0x7feb352du;
    v ^= v >> 15u; v *= 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

static float2 spgiJitterOffset01(uint probeIdx, float frameSeed) {
    if (frameSeed < 0.0f) return float2(0.0f);
    uint h = spgiJitterHash(uint(frameSeed) ^ (probeIdx * 0x9e3779b9u));
    return float2(float(h & 0xffffu), float((h >> 16u) & 0xffffu))
         * (2.0f / 65535.0f) - 1.0f;
}

// ============================================================================
// Main kernel
// ============================================================================

kernel void screen_probe_place(
    // Input textures
    texture2d<float, access::sample>  depthTexture  [[texture(0)]],
    texture2d<float, access::sample>  normalTexture [[texture(1)]],

    // Output buffers
    device float4* probePositions  [[buffer(0)]],
    device float4* probeNormals    [[buffer(1)]],

    // Constant buffer
    constant ScreenProbeGlobalData& global [[buffer(3)]],

    // Thread positioning (1D to be compatible with shared Metal compute encoder)
    uint global_id [[thread_position_in_grid]])
{
    uint gridW = uint(global.grid_params.x);
    uint gridH = uint(global.grid_params.y);
    uint totalProbes = gridW * gridH;

    if (global_id >= totalProbes) return;

    // Compute 2D grid coordinates from 1D index
    uint gx = global_id % gridW;
    uint gy = global_id / gridW;

    float downsample = global.grid_params.z;
    float renderW = global.trace_params.z;
    float renderH = global.trace_params.w;

    uint probeIdx = global_id;

    // Screen-space center of this probe
    float2 pixelCenter = float2(gx * downsample + downsample * 0.5f,
                                gy * downsample + downsample * 0.5f);

    // ------------------------------------------------------------------
    // F5 (M7) per-frame position jitter: quarter-cell screen offset,
    // world-space clamped so the Temporal pass's position-consistency gate
    // never rejects a probe solely because it moved by the jitter.
    // camera_position.w = frame seed (< 0 disables);
    // surface_cache_params.w = jitter_max_world (<= 0 disables).
    // ------------------------------------------------------------------
    float frameSeed = global.camera_position.w;
    float jitterMaxWorld = global.surface_cache_params.w;
    float2 jitterPx = (frameSeed >= 0.0f && jitterMaxWorld > 0.0f)
        ? spgiJitterOffset01(probeIdx, frameSeed) * (downsample * 0.25f)
        : float2(0.0f);

    float2 pixel = pixelCenter + jitterPx;

    // UV coordinates [0, 1]
    float2 uv = pixel / float2(renderW, renderH);

    // Sample depth with POINT filtering — linear interpolation on depth
    // produces ghost values at object edges, creating floating probes
    sampler pointSamp(coord::normalized, filter::nearest, address::clamp_to_edge);
    sampler linearSamp(coord::normalized, filter::linear, address::clamp_to_edge);
    float depth = depthTexture.sample(pointSamp, uv).r;

    // World-space amplitude clamp: nearby surfaces turn a few pixels of
    // screen offset into a large world offset — shrink the jitter (and
    // re-sample) so it stays under the temporal gate budget.
    if (any(jitterPx != float2(0.0f))) {
        float2 uvC = pixelCenter / float2(renderW, renderH);
        float depthC = depthTexture.sample(pointSamp, uvC).r;
        float3 worldC = reconstructWorldPos(uvC, depthC, global.inv_view_projection,
                                            float2(renderW, renderH));
        float3 worldJ = reconstructWorldPos(uv, depth, global.inv_view_projection,
                                            float2(renderW, renderH));
        float delta = distance(worldC, worldJ);
        if (delta > jitterMaxWorld) {
            pixel = pixelCenter + jitterPx * (jitterMaxWorld / max(delta, 1e-5f));
            uv = pixel / float2(renderW, renderH);
            depth = depthTexture.sample(pointSamp, uv).r;
        }
    }

    // Check for sky / far plane
    bool active = (depth > 0.0001f && depth < 0.999f);

    // ------------------------------------------------------------------
    // T5.3 two-level adaptive grid + F4 (M7): fine-tier activation by the
    // UE-style interpolation-failure test (bilinear reconstruction from the
    // 4 edge-neighbor COARSE probes vs the actual depth-reprojected
    // position). cache_params.z <= 0 falls back to the variance heuristic.
    // ------------------------------------------------------------------
    if (active && ((gx + gy) & 1u) != 0u) {
        bool highVariance;
        float posErrThresh = global.cache_params.z;
        if (posErrThresh > 0.0f) {
            float2 uvC = pixel / float2(renderW, renderH);
            float3 posC = reconstructWorldPos(uvC, depth, global.inv_view_projection,
                                              float2(renderW, renderH));
            int2 maxTexel = int2(int(renderW) - 1, int(renderH) - 1);
            int2 ncell[4] = { int2(int(gx) - 1, int(gy)), int2(int(gx) + 1, int(gy)),
                              int2(int(gx), int(gy) - 1), int2(int(gx), int(gy) + 1) };
            bool interpFailed = false;
            float3 acc = float3(0.0f);
            float wsum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                int2 cc = ncell[i];
                if (cc.x < 0 || cc.y < 0 || cc.x >= int(gridW) || cc.y >= int(gridH)) continue;
                int2 tex = clamp(int2(cc.x * int(downsample) + int(downsample) / 2,
                                      cc.y * int(downsample) + int(downsample) / 2),
                                 int2(0), maxTexel);
                float d = depthTexture.read(uint2(tex)).r;
                if (d <= 0.0001f || d >= 0.999f) {
                    interpFailed = true;   // sky coarse neighbor → activate
                    break;
                }
                float2 uvN = (float2(tex) + 0.5f) / float2(renderW, renderH);
                acc += reconstructWorldPos(uvN, d, global.inv_view_projection,
                                           float2(renderW, renderH)) * 0.25f;
                wsum += 0.25f;
            }
            if (!interpFailed && wsum > 0.0f) {
                float scale = 1.0f + (1.0f - wsum);
                interpFailed = distance(acc / wsum, posC) > posErrThresh * scale;
            }
            highVariance = interpFailed || (wsum <= 0.0f);
        } else {
            uint2 base = uint2(clamp(uv * float2(renderW, renderH), float2(0.0f),
                                     float2(renderW - 1.0f, renderH - 1.0f)));
            int2 halfBlock = int2(int(downsample) / 2);
            int2 maxTexel = int2(int(renderW) - 1, int(renderH) - 1);
            float3 nC = normalize(normalTexture.read(base).rgb * 2.0f - 1.0f);
            float dMin = depth, dMax = depth;
            float minNDot = 1.0f;
            int2 cardinals[4] = {
                int2(base) + int2(halfBlock.x, 0), int2(base) - int2(halfBlock.x, 0),
                int2(base) + int2(0, halfBlock.y), int2(base) - int2(0, halfBlock.y)};
            for (int i = 0; i < 4; ++i) {
                int2 t = clamp(cardinals[i], int2(0), maxTexel);
                float d = depthTexture.read(uint2(t)).r;
                dMin = min(dMin, d); dMax = max(dMax, d);
                float3 n = normalize(normalTexture.read(uint2(t)).rgb * 2.0f - 1.0f);
                minNDot = min(minNDot, dot(nC, n));
            }
            highVariance = (dMax - dMin > 0.0015f) || (minNDot < 0.8f);
        }
        if (!highVariance) active = false;
    }

    if (!active) {
        // Mark as inactive: position = (0,0,0,0), normal = (0,0,0,0)
        probePositions[probeIdx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        probeNormals[probeIdx]   = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Reconstruct world position
    float3 worldPos = reconstructWorldPos(uv, depth, global.inv_view_projection, float2(renderW, renderH));

    // Sample normal (world-space, encoded as RGB)
    float3 normal = normalTexture.sample(linearSamp, uv).rgb;
    normal = normalize(normal * 2.0f - 1.0f);  // Decode from [0,1] to [-1,1]

    // Write outputs: w = 1.0 = active flag
    probePositions[probeIdx] = float4(worldPos, 1.0f);
    probeNormals[probeIdx]   = float4(normal, 1.0f);
}
