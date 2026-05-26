// === DDGI GI Gather Shader (8-Probe Trilinear + L2 SH + Cross Bilateral) ===
// Architecture for Apple Silicon:
//   - ALL 64 threads cooperatively load irradiance (72 float3) and depth (1024 floats)
//   - Phase 1: per-pixel trilinear interpolation (probe data from TG)
//   - Phase 2: 4-neighbor cross bilateral (half precision, rational depth weight)
//     - 4 neighbors × 2 TG reads = 8 variable-index TG reads
//     - Uses half3 to reduce register pressure
//     - Rational 1/(1+k*x²) instead of exp() — no transcendental in filter loop
//   - Phase 3: temporal accumulation
//   - Per-thread device reads: ~18 (load) + ~8 (bilateral) + 1 (confidence) + 3 (tex) ≈ 30
//   Total per-thread accesses: ≤30 (within Apple Silicon ~32 limit)

#include <metal_stdlib>
using namespace metal;

struct StaticProbeData {
    float4 GridOrigin;
    uint3  GridCounts;
    float  Spacing;
    float3 _pad0;
    float4 SkySH[9];
};

#define TG_PROBE_COUNT 8
#define SH_COEFF_COUNT 9
#define TILE_SIZE 8
#define DEPTH_RES 8
#define DEPTH_TEXELS 64
#define DEPTH_FLOATS_PER_PROBE (DEPTH_TEXELS * 2u)

struct TGProbe {
    float3 sh[SH_COEFF_COUNT];
};

static float2 gatherOctEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 uv = d.xy / l1norm;
    if (d.z < 0.0f) {
        uv = (1.0f - abs(uv.yx)) * select(float2(-1.0f), float2(1.0f), uv.xy >= 0.0f);
    }
    return uv * 0.5f + 0.5f;
}

static float2 sampleOctDepthTG(
    threadgroup float* tg_depth_map,
    float3 dir)
{
    float2 uv = gatherOctEncode(dir);
    uint2 texel = uint2(clamp(uint(uv.x * float(DEPTH_RES)), 0u, DEPTH_RES - 1u),
                        clamp(uint(uv.y * float(DEPTH_RES)), 0u, DEPTH_RES - 1u));
    uint idx = texel.y * DEPTH_RES + texel.x;
    return float2(tg_depth_map[idx], tg_depth_map[DEPTH_TEXELS + idx]);
}

static float3 evalSH9(threadgroup float3 sh[SH_COEFF_COUNT], float3 d) {
    float x = d.x, y = d.y, z = d.z;
    float x2 = x*x, y2 = y*y, z2 = z*z;
    return sh[0] * 0.28209479177f
         + sh[1] * (0.48860251190f * y)
         + sh[2] * (0.48860251190f * z)
         + sh[3] * (0.48860251190f * x)
         + sh[4] * (1.09254843059f * y * x)
         + sh[5] * (1.09254843059f * y * z)
         + sh[6] * (1.09254843059f * z * x)
         + sh[7] * (0.31539156525f * (3.0f * z2 - 1.0f))
         + sh[8] * (0.54627421530f * (x2 - y2));
}

static float3 shDot9Float4(constant float4 sh[9], float3 d) {
    float3 r = sh[0].xyz * 0.28209479177f;
    r += sh[1].xyz * (0.48860251190f * d.x);
    r += sh[2].xyz * (0.48860251190f * d.y);
    r += sh[3].xyz * (0.48860251190f * d.z);
    float x2 = d.x * d.x, y2 = d.y * d.y, z2 = d.z * d.z;
    r += sh[4].xyz * (1.09254843059f * d.x * d.y);
    r += sh[5].xyz * (1.09254843059f * d.y * d.z);
    r += sh[6].xyz * (1.09254843059f * d.z * d.x);
    r += sh[7].xyz * (0.31539156525f * (3.0f * z2 - 1.0f));
    r += sh[8].xyz * (0.54627421530f * (x2 - y2));
    return r;
}

static float visibilityWeight(float distToProbe, float mean, float variance, float spacing) {
    float dist = max(distToProbe - mean, 0.0f);
    if (variance < 0.0001f) return 1.0f;
    float cheb = variance / (variance + dist * dist);
    return saturate(max(cheb * 2.0f, 0.2f));
}

// === Main kernel ===

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],
    uint2 tidInGroup [[thread_position_in_threadgroup]],
    uint2 groupSize [[threads_per_threadgroup]],

    depth2d<float, access::sample> depthTex [[texture(0)]],
    texture2d<float, access::sample> normalTex [[texture(1)]],
    texture2d<float, access::write> outputTex [[texture(2)]],
    texture2d<float, access::sample> historyTex [[texture(3)]],

    constant float4* viewCB                [[buffer(0)]],
    constant float4& probeOriginSpacing    [[buffer(1)]],
    constant float4& probeCountsCB         [[buffer(2)]],
    device const float3* staticSkySHBuffer [[buffer(3)]],
    device const float* staticSkyFactor    [[buffer(4)]],
    constant StaticProbeData& staticProbe  [[buffer(5)]],
    device const float* confidenceBuffer   [[buffer(6)]],
    device const float3* irradianceBuffer  [[buffer(7)]],
    device const float* ddgiDepthBuffer    [[buffer(8)]]
) {
    uint width = outputTex.get_width();
    uint height = outputTex.get_height();
    uint localIdx = tidInGroup.y * groupSize.x + tidInGroup.x;

    // ========================================================================
    // Phase 0: Cooperative loading — ALL threads participate, NO early returns
    // ========================================================================
    threadgroup TGProbe tg_probes[TG_PROBE_COUNT];
    threadgroup float tg_depth_map[TG_PROBE_COUNT * DEPTH_FLOATS_PER_PROBE];
    threadgroup uint3 tg_cellMin;
    threadgroup bool tg_cellFound;
    threadgroup uint tg_probe_idx[TG_PROBE_COUNT];     // linear probe indices
    threadgroup uint tg_probe_depth_base[TG_PROBE_COUNT];
    threadgroup float tg_skyFactor[TG_PROBE_COUNT];
    threadgroup float3 tg_indirect[TILE_SIZE * TILE_SIZE];
    threadgroup float  tg_pixelDepth[TILE_SIZE * TILE_SIZE];

    // Per-pixel state
    bool inBounds = (tid.x < width && tid.y < height);
    float depth = 0.0f;
    float2 uv = float2(0.0f);
    float3 worldPos = float3(0.0f);
    float3 normal = float3(0.0f, 1.0f, 0.0f);
    float3 gp = float3(0.0f);
    bool active = false;

    if (inBounds) {
        uv = (float2(tid) + 0.5f) / float2(width, height);
        depth = depthTex.sample(sampler(address::clamp_to_edge), uv);
        if (depth < 1.0f) {
            float4x4 invVP = float4x4(viewCB[0], viewCB[1], viewCB[2], viewCB[3]);
            float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 worldPosH = invVP * float4(ndc, depth, 1.0f);
            worldPos = worldPosH.xyz / worldPosH.w;

            float3 n = normalTex.sample(sampler(address::clamp_to_edge), uv).xyz;
            n = normalize(n * 2.0f - 1.0f);
            if (isnormal(n.x) && isnormal(n.y) && isnormal(n.z))
                normal = n;

            float3 origin = probeOriginSpacing.xyz;
            float spacing = probeOriginSpacing.w;
            gp = (worldPos - origin) / spacing;

            float3 gridMax = float3(float(probeCountsCB.x - 1.0f),
                                     float(probeCountsCB.y - 1.0f),
                                     float(probeCountsCB.z - 1.0f));
            active = all(gp >= float3(0.0f)) && all(gp <= gridMax);
        }
    }

    // Determine grid cell from first active thread
    if (localIdx == 0u) {
        tg_cellFound = false;
        tg_cellMin = uint3(0u);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (localIdx < 8u && active && !tg_cellFound) {
        tg_cellMin = uint3(clamp(floor(gp), float3(0.0f),
                                  float3(uint(probeCountsCB.x) - 1u,
                                         uint(probeCountsCB.y) - 1u,
                                         uint(probeCountsCB.z) - 1u)));
        tg_cellFound = true;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint3 counts = uint3(uint(probeCountsCB.x), uint(probeCountsCB.y), uint(probeCountsCB.z));

    // ------------------------------------------------------------------
    // Phase 0a: Threads 0-7 compute probe indices + depth offsets
    // Device reads: 0 (just index computation)
    // ------------------------------------------------------------------
    if (localIdx < 8u) {
        uint cx = (localIdx & 1u) ? min(tg_cellMin.x + 1u, counts.x - 1u) : tg_cellMin.x;
        uint cy = (localIdx & 2u) ? min(tg_cellMin.y + 1u, counts.y - 1u) : tg_cellMin.y;
        uint cz = (localIdx & 4u) ? min(tg_cellMin.z + 1u, counts.z - 1u) : tg_cellMin.z;
        uint pIdx = cx + cy * counts.x + cz * counts.x * counts.y;
        tg_probe_idx[localIdx] = pIdx;
        tg_probe_depth_base[localIdx] = pIdx * DEPTH_FLOATS_PER_PROBE;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ------------------------------------------------------------------
    // Phase 0b: ALL 64 threads cooperatively load irradiance (72 float3)
    // 72 values / 64 threads: threads 0-7 load 2, threads 8-63 load 1
    // Threads 0-7 also load skyFactor (1 read each)
    // ------------------------------------------------------------------
    // First load: threads 0-63 load index [0..63] → probes 0-7, sh 0-7
    {
        uint probe = localIdx / 9u;
        uint shIdx = localIdx % 9u;
        if (probe < TG_PROBE_COUNT) {
            uint pIdx = tg_probe_idx[probe];
            tg_probes[probe].sh[shIdx] = irradianceBuffer[pIdx * 9u + shIdx];
        }
    }
    // Second load: threads 0-7 load index [64..71] → probe 7, sh 1-8
    if (localIdx < 8u) {
        uint idx = 64u + localIdx;  // 64..71
        uint probe = idx / 9u;     // 7 (64/9=7)
        uint shIdx = idx % 9u;     // 1..8
        if (probe < TG_PROBE_COUNT && shIdx < SH_COEFF_COUNT) {
            uint pIdx = tg_probe_idx[probe];
            tg_probes[probe].sh[shIdx] = irradianceBuffer[pIdx * 9u + shIdx];
        }
        // Also load skyFactor (threads 0-7, 1 read each)
        tg_skyFactor[localIdx] = staticSkyFactor[tg_probe_idx[localIdx]];
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ------------------------------------------------------------------
    // Phase 0c: ALL 64 threads cooperatively load depth (scalar reads)
    // 8 probes × 128 floats = 1024 / 64 threads = 16 reads per thread
    // ------------------------------------------------------------------
    for (uint j = 0u; j < 16u; ++j) {
        uint linearIdx = localIdx * 16u + j;  // 0..1023
        uint probe = linearIdx / DEPTH_FLOATS_PER_PROBE;     // 0..7
        uint offsetInProbe = linearIdx % DEPTH_FLOATS_PER_PROBE;  // 0..127
        float val = ddgiDepthBuffer[tg_probe_depth_base[probe] + offsetInProbe];
        tg_depth_map[probe * DEPTH_FLOATS_PER_PROBE + offsetInProbe] = val;
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ========================================================================
    // Phase 1: Per-pixel trilinear interpolation
    // ========================================================================
    if (inBounds && !active) {
        outputTex.write(float4(0.0f), tid);
    }

    if (active) {
        float spacing = probeOriginSpacing.w;
        float3 fracGp = fract(gp);
        float fx = fracGp.x, fy = fracGp.y, fz = fracGp.z;

        float tw[8];
        tw[0] = (1.0f - fx) * (1.0f - fy) * (1.0f - fz);
        tw[1] = fx           * (1.0f - fy) * (1.0f - fz);
        tw[2] = (1.0f - fx) * fy           * (1.0f - fz);
        tw[3] = fx           * fy           * (1.0f - fz);
        tw[4] = (1.0f - fx) * (1.0f - fy) * fz;
        tw[5] = fx           * (1.0f - fy) * fz;
        tw[6] = (1.0f - fx) * fy           * fz;
        tw[7] = fx           * fy           * fz;

        float3 biasedPos = worldPos + normal * spacing * 0.05f;

        float3 dynamicGI = float3(0.0f);
        float  dynTotalWeight = 0.0f;
        float3 bestIrradiance = float3(0.0f);
        float  bestWeight = 0.0f;
        uint   bestProbeIdx = 0u;

        for (uint p = 0u; p < 8u; ++p) {
            if (tw[p] < 0.05f) continue;

            uint cx = (p & 1u) ? min(tg_cellMin.x + 1u, counts.x - 1u) : tg_cellMin.x;
            uint cy = (p & 2u) ? min(tg_cellMin.y + 1u, counts.y - 1u) : tg_cellMin.y;
            uint cz = (p & 4u) ? min(tg_cellMin.z + 1u, counts.z - 1u) : tg_cellMin.z;
            float3 probePos = float3(float(cx), float(cy), float(cz)) * spacing
                              + probeOriginSpacing.xyz;

            float3 toProbe = normalize(probePos - biasedPos);
            float ndotd = dot(normal, toProbe);
            float normalWeight = saturate((ndotd + 0.5f) / 0.7f);

            float distToProbe = length(biasedPos - probePos);

            float2 depthSample = sampleOctDepthTG(&tg_depth_map[p * DEPTH_FLOATS_PER_PROBE], toProbe);
            float meanD = depthSample.x;
            float varD  = depthSample.y;

            float insidePenalty = 1.0f;
            if (meanD < spacing * 0.1f)
                insidePenalty = smoothstep(0.0f, 0.1f, meanD / spacing);

            float dynVis = visibilityWeight(distToProbe, meanD, varD, spacing);
            float dw = tw[p] * normalWeight * dynVis * insidePenalty;

            float3 dynIrr = evalSH9(tg_probes[p].sh, normal);
            if (tw[p] > bestWeight) { bestWeight = tw[p]; bestIrradiance = dynIrr; bestProbeIdx = p; }

            dynamicGI += dynIrr * dw;
            dynTotalWeight += dw;
        }

        if (dynTotalWeight > 0.0f) dynamicGI /= dynTotalWeight;
        else dynamicGI = bestIrradiance * 0.3f;
        dynamicGI = max(dynamicGI, float3(0.0f));

        // Confidence — single device read
        float bestConvergenceAge = confidenceBuffer[tg_probe_idx[bestProbeIdx] * 4u + 3u];
        float confidence = saturate(bestConvergenceAge);

        float skyF = 0.0f, sfTotalW = 0.0f;
        for (uint i = 0u; i < 8u; ++i) {
            if (tw[i] < 0.05f) continue;
            skyF += tg_skyFactor[i] * tw[i];
            sfTotalW += tw[i];
        }
        skyF = (sfTotalW > 0.0f) ? skyF / sfTotalW : 0.0f;
        float3 skyFallback = shDot9Float4(staticProbe.SkySH, normal);

        float3 indirect;
        if (confidence >= 0.7f) {
            indirect = dynamicGI;
        } else {
            float blendFactor = smoothstep(0.1f, 0.7f, confidence);
            indirect = mix(skyFallback, dynamicGI, blendFactor);
        }

        uint tgIdx = tidInGroup.y * TILE_SIZE + tidInGroup.x;
        tg_indirect[tgIdx] = indirect;
        tg_pixelDepth[tgIdx] = depth;
    } else {
        uint tgIdx = tidInGroup.y * TILE_SIZE + tidInGroup.x;
        tg_indirect[tgIdx] = float3(0.0f);
        tg_pixelDepth[tgIdx] = 1.0f;
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ========================================================================
    // Phase 2: Cross bilateral filter (4 cardinal neighbors, depth-aware)
    // Optimizations per Apple Silicon best practices:
    //   - Cross pattern (not 5×5): 4 neighbors × 2 TG reads = 8 variable-index
    //   - half precision: reduces register pressure, avoids spill
    //   - Rational weight 1/(1+k*x²) instead of exp(): no transcendental in loop
    //   - Unrolled: fixed +/- TILE_SIZE and +/- 1 offsets
    // Per-thread variable-index TG reads: ≤8 (total device reads ≈ 26-30)
    // ========================================================================
    if (active) {
        uint tgIdx = tidInGroup.y * TILE_SIZE + tidInGroup.x;
        half3 indirect = half3(tg_indirect[tgIdx]);
        half  myDepth  = half(tg_pixelDepth[tgIdx]);

        half3 filterSum = indirect;
        half  weightSum = 1.0h;
        const half kDepthFalloff = 8.0h;

        // Up (y-1)
        if (tidInGroup.y > 0u) {
            uint ni = tgIdx - TILE_SIZE;
            half dd = half(tg_pixelDepth[ni]) - myDepth;
            half w = 1.0h / (1.0h + kDepthFalloff * dd * dd);
            filterSum += half3(tg_indirect[ni]) * w;
            weightSum += w;
        }
        // Down (y+1)
        if (tidInGroup.y < TILE_SIZE - 1u) {
            uint ni = tgIdx + TILE_SIZE;
            half dd = half(tg_pixelDepth[ni]) - myDepth;
            half w = 1.0h / (1.0h + kDepthFalloff * dd * dd);
            filterSum += half3(tg_indirect[ni]) * w;
            weightSum += w;
        }
        // Left (x-1)
        if (tidInGroup.x > 0u) {
            uint ni = tgIdx - 1u;
            half dd = half(tg_pixelDepth[ni]) - myDepth;
            half w = 1.0h / (1.0h + kDepthFalloff * dd * dd);
            filterSum += half3(tg_indirect[ni]) * w;
            weightSum += w;
        }
        // Right (x+1)
        if (tidInGroup.x < TILE_SIZE - 1u) {
            uint ni = tgIdx + 1u;
            half dd = half(tg_pixelDepth[ni]) - myDepth;
            half w = 1.0h / (1.0h + kDepthFalloff * dd * dd);
            filterSum += half3(tg_indirect[ni]) * w;
            weightSum += w;
        }

        half3 filtered = filterSum / weightSum;

        // ========================================================================
        // Phase 3: Temporal accumulation
        // ========================================================================
        float3 histGI = historyTex.sample(sampler(address::clamp_to_edge), uv).xyz;
        float temporalAlpha = 0.3f;
        if (all(histGI == float3(0.0f))) {
            temporalAlpha = 1.0f;
        }
        float3 result = mix(histGI, float3(filtered), temporalAlpha);
        result = max(result, float3(0.03f, 0.03f, 0.035f));
        outputTex.write(float4(result, 1.0f), tid);
    }
}
