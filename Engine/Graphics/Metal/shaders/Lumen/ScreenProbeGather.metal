/**
 * @file ScreenProbeGather.metal
 * @brief Screen Probe GI - Visibility-constrained log-space interpolation
 *
 * Core principle: never interpolate across visibility boundaries.
 *
 * Visibility constraints (all must pass for a probe to contribute):
 *   1. Pixel→Probe: dot(pixelNormal, probeDir) > 0  (probe is above pixel horizon)
 *   2. Probe→Pixel: dot(probeNormal, pixelDir) > 0   (pixel is above probe horizon)
 *   3. Depth layer: screen-space depth difference is small (same surface)
 *   4. World distance: probe is close enough to influence this pixel
 *
 * Interpolation: log-space weighted average prevents HDR dominance.
 * Firefly clamping happens at probe level (Average), not here.
 *
 * Output: float4(irradiance * GI_INTENSITY, confidence)
 *
 * Buffer reads per pixel: 4(pos) + 4(normal) + 4×4(SH) = 24 total
 *
 * Dispatch: (renderW * renderH + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
using namespace metal;

struct ScreenProbeGlobalData {
    float4x4 view_projection;
    float4x4 inv_view_projection;
    float4   camera_position;

    float4   grid_params;     // x=gridW, y=gridH, z=downsample, w=raysPerProbe
    float4   trace_params;    // x=maxRayDist, y=gatherRadius, z=renderWidth, w=renderHeight

    float4   sdf_origins[3];
    float4   sdf_voxel_sizes[3];
    float4   sdf_extents[3];
    float4   sdf_resolutions;
};

static float4 shBasis(float3 d) {
    return float4(
        0.282095f,
        0.488603f * d.y,
        0.488603f * d.z,
        0.488603f * d.x
    );
}

static float3 reconstructWorldPos(float2 uv, float depth,
                                  constant float4x4& invVP) {
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = invVP * clipPos;
    return worldPos.xyz / worldPos.w;
}

static float luminance(float3 c) {
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Evaluate irradiance from 4 SH coefficients at a given normal
static float3 evalSH(float3 sh0, float3 sh1, float3 sh2, float3 sh3, float4 Y) {
    float3 irr = sh0 * Y.x + sh1 * Y.y + sh2 * Y.z + sh3 * Y.w;
    return max(irr, float3(0.0f));
}

// Result from probe gathering
struct GatherResult {
    float3 logGI;
    float  totalWeight;
    float  bilinearSum;
};

static GatherResult gatherProbes(
    int2 baseProbe, float2 frac,
    uint gridW, uint gridH,
    device const float4* probePositions,
    device const float4* probeNormals,
    device const float4* probeSH,
    float3 worldPos, float3 normal, float pixelDepthNDC,
    float4 Y_normal, constant float4x4& viewProjection,
    bool constrained)
{
    GatherResult result;
    result.logGI = float3(0.0f);
    result.totalWeight = 0.0f;
    result.bilinearSum = 0.0f;

    constexpr float DEPTH_SIGMA = 0.8f;       // world-space plane distance
    constexpr float NDC_DEPTH_THRESHOLD = 0.02f; // screen-space depth layer threshold
    constexpr float WORLD_DIST_SIGMA = 8.0f;  // world-space distance falloff
    constexpr float MAX_RADIANCE = 8.0f;
    constexpr float BRIGHT_THRESHOLD = 2.0f;

    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            int kx = baseProbe.x + dx;
            int ky = baseProbe.y + dy;

            if (kx < 0 || kx >= int(gridW) || ky < 0 || ky >= int(gridH)) continue;

            uint pidx = uint(ky) * gridW + uint(kx);

            float4 posData = probePositions[pidx];
            if (posData.w <= 0.0f) continue;

            uint shBase = pidx * 4;
            if (probeSH[shBase + 0].w <= 0.0f) continue;

            float3 probePos = posData.xyz;

            // Bilinear weight
            float wx = (dx == 0) ? (1.0f - frac.x) : frac.x;
            float wy = (dy == 0) ? (1.0f - frac.y) : frac.y;
            float bilinear = wx * wy;

            result.bilinearSum += bilinear;

            float weight = bilinear;

            if (constrained) {
                // ===== Visibility gate 1: pixel → probe (above horizon) =====
                float3 probeDir = normalize(probePos - worldPos);
                float cosPixelToProbe = dot(normal, probeDir);
                if (cosPixelToProbe < 0.0f) continue;  // hard reject: probe is behind surface

                // ===== Visibility gate 2: probe → pixel (reciprocal) =====
                float4 normData = probeNormals[pidx];
                if (normData.w > 0.0f) {
                    float3 probeNormal = normalize(normData.xyz);
                    float3 pixelDir = normalize(worldPos - probePos);
                    float cosProbeToPixel = dot(probeNormal, pixelDir);
                    if (cosProbeToPixel < 0.0f) continue;  // hard reject: pixel behind probe's surface
                }

                // ===== Visibility gate 3: depth layer (screen-space) =====
                float4 probeClipPos = viewProjection * float4(probePos, 1.0f);
                float probeDepthNDC = probeClipPos.z / probeClipPos.w;
                float ndcDepthDiff = abs(pixelDepthNDC - probeDepthNDC);
                if (ndcDepthDiff > NDC_DEPTH_THRESHOLD) continue;  // hard reject: different depth layer

                // ===== World-space distance penalty =====
                float worldDist = distance(probePos, worldPos);
                float w_dist = exp(-worldDist * worldDist / (WORLD_DIST_SIGMA * WORLD_DIST_SIGMA));
                weight *= w_dist;

                // ===== Plane distance (soft) =====
                float planeDist = abs(dot(probePos - worldPos, normal));
                float w_plane = exp(-planeDist * planeDist / (DEPTH_SIGMA * DEPTH_SIGMA));
                weight *= w_plane;

                // ===== Horizon soft transition =====
                float w_visibility = smoothstep(0.0f, 0.3f, cosPixelToProbe);
                weight *= w_visibility;
            }

            // Evaluate SH at pixel normal → irradiance
            float3 probeIrr = evalSH(
                probeSH[shBase + 0].rgb, probeSH[shBase + 1].rgb,
                probeSH[shBase + 2].rgb, probeSH[shBase + 3].rgb, Y_normal);

            // Residual bright clamp (probe-level clamping in Average handles most)
            float lum = luminance(probeIrr);
            if (lum > MAX_RADIANCE) {
                probeIrr *= MAX_RADIANCE / lum;
                lum = MAX_RADIANCE;
            }

            // Bright probe penalty
            if (lum > BRIGHT_THRESHOLD) {
                weight *= exp(-(lum - BRIGHT_THRESHOLD) * 0.5f);
            }

            // Accumulate in log space
            float3 logIrr = log(1.0f + probeIrr);
            result.logGI += logIrr * weight;
            result.totalWeight += weight;
        }
    }

    return result;
}

kernel void screen_probe_gather(
    texture2d<float, access::sample>  depthTexture  [[texture(0)]],
    texture2d<float, access::sample>  normalTexture [[texture(1)]],
    texture2d<float, access::write>   outputTexture [[texture(2)]],

    device const float4* probePositions   [[buffer(0)]],
    device const float4* probeSH          [[buffer(2)]],
    constant ScreenProbeGlobalData& global [[buffer(3)]],
    device const float4* probeNormals     [[buffer(4)]],

    uint global_id [[thread_position_in_grid]])
{
    uint renderW = uint(global.trace_params.z);
    uint renderH = uint(global.trace_params.w);
    uint totalPixels = renderW * renderH;

    if (global_id >= totalPixels) return;

    uint px = global_id % renderW;
    uint py = global_id / renderW;
    uint2 gid = uint2(px, py);

    uint  gridW = uint(global.grid_params.x);
    uint  gridH = uint(global.grid_params.y);
    float downsample = global.grid_params.z;

    float2 uv = float2(float(px) + 0.5f, float(py) + 0.5f) / float2(renderW, renderH);

    sampler pointSamp(coord::normalized, filter::nearest, address::clamp_to_edge);
    sampler linearSamp(coord::normalized, filter::linear, address::clamp_to_edge);

    float depth = depthTexture.sample(pointSamp, uv).r;

    if (depth <= 0.0001f || depth >= 0.999f) {
        outputTexture.write(float4(0.0f, 0.0f, 0.0f, 0.0f), gid);
        return;
    }

    float3 worldPos = reconstructWorldPos(uv, depth, global.inv_view_projection);
    float3 normal = normalize(normalTexture.sample(linearSamp, uv).rgb * 2.0f - 1.0f);

    float4 Y_normal = shBasis(normal);

    float2 gridPos = float2(float(px), float(py)) / downsample;
    int2 baseProbe = int2(floor(gridPos));
    float2 frac = gridPos - float2(baseProbe);

    // =====================================================================
    // Phase 1: Visibility-constrained interpolation
    // =====================================================================
    GatherResult result = gatherProbes(
        baseProbe, frac, gridW, gridH,
        probePositions, probeNormals, probeSH,
        worldPos, normal, depth, Y_normal,
        global.view_projection,
        true);

    // =====================================================================
    // Phase 2: Fallback — relax to pure bilinear (no visibility gates)
    // =====================================================================
    bool usedFallback = false;
    constexpr float MIN_WEIGHT_SUM = 0.01f;

    if (result.totalWeight < MIN_WEIGHT_SUM) {
        usedFallback = true;
        result = gatherProbes(
            baseProbe, frac, gridW, gridH,
            probePositions, probeNormals, probeSH,
            worldPos, normal, depth, Y_normal,
            global.view_projection,
            false);  // no visibility constraints
    }

    // =====================================================================
    // Confidence + output
    // =====================================================================
    float confidence = 0.0f;
    if (result.bilinearSum > 0.01f) {
        confidence = saturate(result.totalWeight / result.bilinearSum);
    }
    if (usedFallback) {
        confidence *= 0.2f;
    }

    // Log-space → linear conversion
    constexpr float WEIGHT_FLOOR = 0.05f;
    float normWeight = max(result.totalWeight, WEIGHT_FLOOR);
    float3 irradiance = float3(0.0f);

    if (result.totalWeight > 0.001f) {
        float3 avgLog = result.logGI / normWeight;
        irradiance = exp(avgLog) - 1.0f;
    }

    constexpr float GI_INTENSITY = 5.0f;
    irradiance *= GI_INTENSITY;

    outputTexture.write(float4(irradiance, confidence), gid);
}
