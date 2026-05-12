/**
 * @file DDGIGIGather.metal
 * @brief Half-resolution DDGI indirect irradiance gathering (compute)
 *
 * Strategy: L0+L1+L2 SH (9 coeff) tetrahedral 4-probe interpolation
 *   - Soft distance falloff with visibility weight from DDGI depth
 *   - Normal hemisphere weight
 *   - Full L0+L1+L2 SH reconstruction
 *
 * Total reads: 36 storage buffer (irradiance) + 4 storage buffer (depth) + 2 texture2D (depth+normal)
 */

#include <metal_stdlib>
using namespace metal;

// SH constants
constant float _C0   = 0.282095f;   // L0
constant float _C1   = 0.488603f;   // L1
constant float _C2_0 = 1.092548f;   // L2
constant float _C2_1 = 0.315392f;
constant float _C2_2 = 0.546274f;

// L0+L1 SH dot product (4 coefficients) — stable with 64 rays
static float3 shDot4(thread const float3* c, float3 d) {
    float x = d.x, y = d.y, z = d.z;
    return c[0] * _C0
         + c[1] * (-_C1 * y)
         + c[2] * ( _C1 * z)
         + c[3] * (-_C1 * x);
}

// L0+L1+L2 SH dot product (9 coefficients)
static float3 shDot9(thread const float3* c, float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    return c[0] * _C0
         + c[1] * (-_C1 * y)
         + c[2] * ( _C1 * z)
         + c[3] * (-_C1 * x)
         + c[4] * ( _C2_0 * y * x)
         + c[5] * (-_C2_0 * y * z)
         + c[6] * ( _C2_1 * (3.0f * z2 - 1.0f))
         + c[7] * (-_C2_0 * x * z)
         + c[8] * ( _C2_2 * (x2 - y2));
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

static float visibilityWeight(float dist, float mean, float variance, float spacing) {
    if (dist <= mean) return 1.0f;
    float d_minus_mean = dist - mean;
    float chebyshev = variance / (variance + d_minus_mean * d_minus_mean);
    float bias = spacing * 0.1f;
    float threshold = mean + bias + chebyshev * spacing;
    float falloff = spacing * 1.0f;
    return saturate((threshold - dist + falloff) / falloff);
}

static float2 octahedralEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 uv = d.xy / l1norm;
    if (d.z < 0.0f) {
        uv = (1.0f - abs(uv.yx)) * select(float2(-1.0f), float2(1.0f), uv.xy >= 0.0f);
    }
    return uv * 0.5f + 0.5f;
}

static float2 sampleDepthOctahedral(device const float* buf, uint probeIdx, float3 dir) {
    constexpr uint res = 8u;
    constexpr uint texels = res * res;
    float2 uv = octahedralEncode(dir);
    uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u),
                        clamp(uint(uv.y * float(res)), 0u, res - 1u));
    uint idx = texel.y * res + texel.x;
    uint probeBase = probeIdx * texels * 2u;
    return float2(buf[probeBase + idx], buf[probeBase + texels + idx]);
}

static uint3 probeGridCoord(uint probeIdx, uint3 counts) {
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    return uint3(px, py, pz);
}

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],

    depth2d<float, access::sample>   depthTex      [[texture(0)]],
    texture2d<float, access::sample> normalTex     [[texture(1)]],
    texture2d<float, access::write>  outputTex     [[texture(2)]],

    constant float4x4& invViewProj        [[buffer(0)]],
    constant float4&    probeOriginSpacing [[buffer(1)]],
    constant float4&    probeCounts        [[buffer(2)]],
    device const float3* irradianceBuffer  [[buffer(3)]],
    device const float*  ddgiDepthBuffer   [[buffer(4)]]
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

    // --- Tetrahedral 4 probes ---
    uint pi[4]; float bw[4];
    tetrahedral(gp, counts, pi, bw);

    float3 result(0.0f);
    float  totalWeight = 0.0f;

    // Normal bias to prevent self-shadowing acne (small to avoid thin wall penetration)
    float3 biasedPos = worldPos + normal * spacing * 0.05f;

    // Track best fallback probe (highest barycentric weight) for guaranteed output
    float3 bestIrradiance(0.0f);
    float  bestWeight = 0.0f;

    for (uint p = 0; p < 4u; ++p) {
        if (bw[p] < 0.001f) continue;

        uint3 gc = probeGridCoord(pi[p], counts);
        float3 probePos = origin + float3(float(gc.x), float(gc.y), float(gc.z)) * spacing;

        float3 toProbe = normalize(probePos - biasedPos);
        float ndotd = dot(normal, toProbe);
        float normalWeight = saturate((ndotd + 0.5f) / 0.7f);

        float3 fromProbeDir = normalize(biasedPos - probePos);
        float distToProbe = length(biasedPos - probePos);

        // Read irradiance L0+L1
        uint base = pi[p] * 9u;
        float3 sh[4];
        for (uint i = 0u; i < 4u; ++i) {
            sh[i] = irradianceBuffer[base + i];
        }
        float3 irradiance = shDot4(sh, normal);

        // Track best probe for guaranteed fallback
        if (bw[p] > bestWeight) {
            bestWeight = bw[p];
            bestIrradiance = irradiance;
        }

        // Octahedral depth sampling
        float2 depthMV = sampleDepthOctahedral(ddgiDepthBuffer, pi[p], fromProbeDir);

        // Soft inside-geometry penalty instead of hard skip:
        // probes with very short mean depth get reduced weight, not zero
        float insidePenalty = 1.0f;
        if (depthMV.x < spacing * 0.2f) {
            insidePenalty = smoothstep(0.0f, 0.2f, depthMV.x / spacing);
        }

        float visWeight = visibilityWeight(distToProbe, depthMV.x, depthMV.y, spacing);

        float w = bw[p] * normalWeight * visWeight * insidePenalty;
        result += irradiance * w;
        totalWeight += w;
    }

    if (totalWeight > 0.0f) {
        result /= totalWeight;
    } else {
        // Fallback: use best probe at reduced strength — always produce SOME indirect light
        result = bestIrradiance * 0.3f;
    }
    result = max(result, float3(0.0f));

    // Minimum ambient floor: prevents completely black shadow areas when
    // probe irradiance data is near-zero (radiance source lacks indirect bounce)
    result = max(result, float3(0.03f, 0.03f, 0.035f));

    outputTex.write(float4(result, 1.0f), tid);
}
