/**
 * @file DDGIGIGather.metal
 * @brief Half-resolution DDGI indirect irradiance gathering (compute)
 *
 * Tetrahedral 4-probe + depth visibility with generous threshold.
 * Reads: 16 storage buffer + 2 texture2D + 1 texture3D.
 */

#include <metal_stdlib>
using namespace metal;

constant float _C0   = 0.282095f;
constant float _C1   = 0.488603f;

static float3 shDot4(thread const float3* c, float3 d) {
    float b0 =  _C0;
    float b1 = -_C1 * d.y;
    float b2 =  _C1 * d.z;
    float b3 = -_C1 * d.x;
    return c[0]*b0 + c[1]*b1 + c[2]*b2 + c[3]*b3;
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

// Isolated function: sample ddgiDepth texture3D
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

constant float3 DEPTH_DIRS[8] = {
    float3( 1, 0, 0), float3(-1, 0, 0),
    float3( 0, 1, 0), float3( 0,-1, 0),
    float3( 0, 0, 1), float3( 0, 0,-1),
    float3(0.57735f, 0.57735f, 0.57735f),
    float3(-0.57735f,-0.57735f,-0.57735f)
};

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],

    depth2d<float, access::sample>   depthTex      [[texture(0)]],
    texture2d<float, access::sample> normalTex     [[texture(1)]],
    texture3d<float, access::sample> ddgiDepthTex  [[texture(2)]],
    texture2d<float, access::write>  outputTex     [[texture(3)]],

    constant float4x4& invViewProj        [[buffer(0)]],
    constant float4&    probeOriginSpacing [[buffer(1)]],
    constant float4&    probeCounts        [[buffer(2)]],
    device const float3* irradianceBuffer  [[buffer(3)]]
)
{
    uint2 outSize = uint2(outputTex.get_width(), outputTex.get_height());
    if (any(tid >= outSize)) return;

    float2 uv = (float2(tid) + 0.5f) / float2(outSize);

    constexpr sampler depthS(coord::normalized, filter::nearest, address::clamp_to_edge);
    constexpr sampler linearS(coord::normalized, filter::linear, address::clamp_to_edge);

    float depth = depthTex.sample(depthS, uv);
    if (depth >= 1.0f) {
        outputTex.write(float4(0.0f), tid);
        return;
    }

    float3 normal = normalize(normalTex.sample(linearS, uv).xyz * 2.0f - 1.0f);

    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 wp = invViewProj * float4(ndc, depth, 1.0f);
    float3 worldPos = wp.xyz / wp.w;

    float3 origin  = probeOriginSpacing.xyz;
    float  spacing = probeOriginSpacing.w;
    uint3  counts  = uint3(probeCounts.xyz);

    float3 gp = (worldPos - origin) / spacing;
    float3 gridMax = float3(float(counts.x - 1u), float(counts.y - 1u), float(counts.z - 1u));

    if (any(gp < 0.0f) || any(gp > gridMax)) {
        outputTex.write(float4(0.0f), tid);
        return;
    }

    uint pi[4]; float bw[4];
    tetrahedral(gp, counts, pi, bw);

    // Depth visibility: single texture3D read for nearest probe
    uint3 nearPC = clamp(uint3(round(gp)), uint3(0u), counts - 1u);
    float3 nearProbePos = origin + float3(float(nearPC.x), float(nearPC.y), float(nearPC.z)) * spacing;

    float3 toSurface = worldPos - nearProbePos;
    float dist = length(toSurface);
    float globalVis = 1.0f;

    if (dist > 0.001f) {
        float3 dir = toSurface / dist;
        float bestDot = -2.0f;
        uint bestDir = 0;
        for (uint i = 0; i < 8u; ++i) {
            float d = dot(dir, DEPTH_DIRS[i]);
            if (d > bestDot) { bestDot = d; bestDir = i; }
        }

        float storedMean = sampleDDGIDepth(ddgiDepthTex, nearPC, counts, bestDir);

        // Generous threshold: stored depth is MIN distance in that octant.
        // Actual surface can be much further. Only reject if >> stored.
        // threshold = stored * 4x + 2 * probeSpacing
        float threshold = storedMean * 4.0f + spacing * 2.0f;
        if (dist > threshold) {
            globalVis = 0.0f;
        } else {
            // Smooth falloff near threshold
            float fadeStart = storedMean * 2.0f + spacing;
            if (dist > fadeStart) {
                globalVis = 1.0f - (dist - fadeStart) / max(threshold - fadeStart, 0.01f);
            }
        }
    }

    float3 result(0.0f);
    float  totalWeight = 0.0f;

    for (uint p = 0; p < 4u; ++p) {
        if (bw[p] < 0.001f) continue;

        uint base = pi[p] * 9u;
        float3 sh[4];
        sh[0] = irradianceBuffer[base + 0u];
        sh[1] = irradianceBuffer[base + 1u];
        sh[2] = irradianceBuffer[base + 2u];
        sh[3] = irradianceBuffer[base + 3u];

        float3 irradiance = shDot4(sh, normal);
        result += irradiance * bw[p];
        totalWeight += bw[p];
    }

    if (totalWeight > 0.0f) result /= totalWeight;
    result *= globalVis;
    result = max(result, float3(0.0f));

    outputTex.write(float4(result, 1.0f), tid);
}
