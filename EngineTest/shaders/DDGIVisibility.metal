/**
 * @file DDGIVisibility.metal
 * @brief DDGI per-probe visibility pre-computation (compute, half-res)
 *
 * Pass 1 of 2-pass split:
 *   - Reads texture3D depth for ALL 4 tetrahedral probes (4 reads)
 *   - NO storage buffer reads (irradiance stays in Pass 2)
 *   - Output: RGBA16_Float (R/G/B/A = visibility for probe 0/1/2/3)
 *
 * Total reads: 2 texture2D + 4 texture3D + 3 uniform CB = safe
 * (No storage buffer contention with texture3D)
 */

#include <metal_stdlib>
using namespace metal;

constant float3 DEPTH_DIRS[8] = {
    float3( 1, 0, 0), float3(-1, 0, 0),
    float3( 0, 1, 0), float3( 0,-1, 0),
    float3( 0, 0, 1), float3( 0, 0,-1),
    float3(0.57735f, 0.57735f, 0.57735f),
    float3(-0.57735f,-0.57735f,-0.57735f)
};

// Isolated function: single texture3D read for probe depth
static float sampleDDGIDepth(
    texture3d<float, access::sample> depthTex,
    uint3 gridCoord, uint3 gridDim, uint dirIdx)
{
    float3 uvw;
    uvw.x = (float(gridCoord.x) + 0.5f) / float(gridDim.x);
    uvw.y = (float(gridCoord.y) + 0.5f) / float(gridDim.y);
    uvw.z = (float(gridCoord.z * 4u + dirIdx) + 0.5f) / float(gridDim.z * 4u);
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    return depthTex.sample(s, uvw).r;
}

static void tetrahedral(float3 gp, uint3 gd,
                        thread uint* pi, thread float* bw) {
    uint3 b0 = clamp(uint3(floor(gp)), uint3(0u), gd - 1u);
    uint3 b1 = min(b0 + 1u, gd - 1u);
    uint p[8];
    p[0]=b0.x+b0.y*gd.x+b0.z*gd.x*gd.y; p[1]=b1.x+b0.y*gd.x+b0.z*gd.x*gd.y;
    p[2]=b0.x+b1.y*gd.x+b0.z*gd.x*gd.y; p[3]=b1.x+b1.y*gd.x+b0.z*gd.x*gd.y;
    p[4]=b0.x+b0.y*gd.x+b1.z*gd.x*gd.y; p[5]=b1.x+b0.y*gd.x+b1.z*gd.x*gd.y;
    p[6]=b0.x+b1.y*gd.x+b1.z*gd.x*gd.y; p[7]=b1.x+b1.y*gd.x+b1.z*gd.x*gd.y;
    float fx=fract(gp.x), fy=fract(gp.y), fz=fract(gp.z);
    if      (fx>=fy&&fy>=fz) { pi[0]=p[0];pi[1]=p[1];pi[2]=p[3];pi[3]=p[7]; bw[0]=1-fx;bw[1]=fx-fy;bw[2]=fy-fz;bw[3]=fz; }
    else if (fx>=fz&&fz>=fy) { pi[0]=p[0];pi[1]=p[1];pi[2]=p[5];pi[3]=p[7]; bw[0]=1-fx;bw[1]=fx-fz;bw[2]=fz-fy;bw[3]=fy; }
    else if (fy>=fx&&fx>=fz) { pi[0]=p[0];pi[1]=p[2];pi[2]=p[3];pi[3]=p[7]; bw[0]=1-fy;bw[1]=fy-fx;bw[2]=fx-fz;bw[3]=fz; }
    else if (fy>=fz&&fz>=fx) { pi[0]=p[0];pi[1]=p[2];pi[2]=p[6];pi[3]=p[7]; bw[0]=1-fy;bw[1]=fy-fz;bw[2]=fz-fx;bw[3]=fx; }
    else if (fz>=fx&&fx>=fy) { pi[0]=p[0];pi[1]=p[4];pi[2]=p[5];pi[3]=p[7]; bw[0]=1-fz;bw[1]=fz-fx;bw[2]=fx-fy;bw[3]=fy; }
    else                     { pi[0]=p[0];pi[1]=p[4];pi[2]=p[6];pi[3]=p[7]; bw[0]=1-fz;bw[1]=fz-fy;bw[2]=fy-fx;bw[3]=fx; }
    for (uint i = 0; i < 4u; ++i) bw[i] = max(bw[i], 0.0f);
}

// Probe grid coord from linear index
static uint3 probeGridCoord(uint probeIdx, uint3 counts) {
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    return uint3(px, py, pz);
}

kernel void ddgi_visibility(
    uint2 tid [[thread_position_in_grid]],

    depth2d<float, access::sample>   depthTex      [[texture(0)]],
    texture2d<float, access::sample> normalTex     [[texture(1)]],
    texture3d<float, access::sample> ddgiDepthTex  [[texture(2)]],
    texture2d<float, access::write>  visibilityTex [[texture(3)]],

    constant float4x4& invViewProj        [[buffer(0)]],
    constant float4&    probeOriginSpacing [[buffer(1)]],
    constant float4&    probeCounts        [[buffer(2)]]
)
{
    uint2 outSize = uint2(visibilityTex.get_width(), visibilityTex.get_height());
    if (any(tid >= outSize)) return;

    float2 uv = (float2(tid) + 0.5f) / float2(outSize);

    constexpr sampler depthS(coord::normalized, filter::nearest, address::clamp_to_edge);

    float depth = depthTex.sample(depthS, uv);
    if (depth >= 1.0f) {
        visibilityTex.write(float4(1.0f), tid);
        return;
    }

    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wp = invViewProj * float4(ndc, depth, 1.0f);
    float3 worldPos = wp.xyz / wp.w;

    float3 origin  = probeOriginSpacing.xyz;
    float  spacing = probeOriginSpacing.w;
    uint3  counts  = uint3(probeCounts.xyz);

    float3 gp = (worldPos - origin) / spacing;
    float3 gridMax = float3(float(counts.x - 1u), float(counts.y - 1u), float(counts.z - 1u));

    if (any(gp < 0.0f) || any(gp > gridMax)) {
        visibilityTex.write(float4(1.0f), tid);
        return;
    }

    // --- Tetrahedral 4 probes ---
    uint pi[4]; float bw[4];
    tetrahedral(gp, counts, pi, bw);

    float4 visibility(1.0f);

    for (uint p = 0; p < 4u; ++p) {
        if (bw[p] < 0.001f) { visibility[p] = 1.0f; continue; }

        uint3 gc = probeGridCoord(pi[p], counts);
        float3 probePos = origin + float3(float(gc.x), float(gc.y), float(gc.z)) * spacing;

        float3 toSurface = worldPos - probePos;
        float dist = length(toSurface);
        if (dist < 0.001f) { visibility[p] = 1.0f; continue; }

        float3 dir = toSurface / dist;

        // Find best matching depth direction
        float bestDot = -2.0f;
        uint bestDir = 0;
        for (uint i = 0; i < 8u; ++i) {
            float d = dot(dir, DEPTH_DIRS[i]);
            if (d > bestDot) { bestDot = d; bestDir = i; }
        }

        float storedMean = sampleDDGIDepth(ddgiDepthTex, gc, counts, bestDir);
        float threshold = storedMean * 3.0f + spacing * 1.5f;
        if (dist > threshold) {
            visibility[p] = 0.0f;
        } else {
            float fadeStart = storedMean * 1.5f + spacing * 0.8f;
            if (dist > fadeStart) {
                visibility[p] = 1.0f - (dist - fadeStart) / max(threshold - fadeStart, 0.01f);
            }
        }
    }

    visibilityTex.write(visibility, tid);
}
