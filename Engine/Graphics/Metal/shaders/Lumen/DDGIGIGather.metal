// === DDGI GI Gather Shader (8-Probe Trilinear SH L0+L1, Vulkan legacy parity) ===
// T8.13 Vulkan↔Metal sync (2026-08-28): body rewritten to the CURRENT Vulkan
// DDGIGIGather.comp legacy route (C) — the path the Vulkan baseline numbers
// (52.5/46.9/97.2/24.7) were measured on:
//   - plain 8-probe trilinear SH (canonical basis, matches the projection).
//     Deviation from the Vulkan SH9 baseline: per-pixel evaluation keeps only
//     L0+L1 (32 buffer reads/pixel stays inside Apple Silicon's practical
//     budget; the cooperative tile path it replaced selected ONE probe cell
//     per 8x8 screen tile and produced jagged grid seams). L2 restauration is
//     pending a cheap-per-pixel load strategy.
//   - NO visibility/normal weighting, NO static-sky fallback, NO confidence
//     gate, NO cross-bilateral, NO temporal accumulation (the Vulkan legacy
//     path has none of these; leak safety lives in the probe content)
//   - diffuse output = radiance × π (PROBE_SH_RAD_TO_IRRAD) with an absolute
//     sky-leak floor (0.05, 0.05, 0.06)
//   - glossy output = R-lobe radiance blended to the diffuse limit by
//     roughness², alpha = NdotV (DFG happens in FusionComposite)
//   - outside the near grid: dim ambient (0.03, 0.03, 0.035) — the far
//     volume's explicit Metal bindings are deferred in GIGatherModule, so the
//     two-stage far fallback stays Vulkan-only for now.
//
// Apple Silicon structure: cooperative threadgroup staging of the 8 surrounding
// probes' SH (72 float3), (8,8) tiles, no early returns before barriers. The
// representative cell is selected deterministically so depth discontinuities
// cannot make the tile's GI choice vary from frame to frame.
//
// PAIRED-CHANGE note (from the Vulkan file): never apply the π factor inside
// SurfaceCacheLightEval's probe gather — its loop gain budget was calibrated
// without it; this correction is for the screen end only.

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

// Screen consumers need cosine-weighted irradiance E(N) = π·L̄; the stored SH
// is RADIANCE (mcWeight = 4π/N). Applied at the diffuse output only.
constant float PROBE_SH_RAD_TO_IRRAD = 3.14159265f;

static float3 evalSH4Canonical(thread const float3 sh[4], float3 d) {
    float x = d.x, y = d.y, z = d.z;
    // L0+L1 keeps the per-pixel path to 32 buffer reads (8 probes × 4
    // coefficients), within Apple Silicon's practical buffer-read budget.
    // The signs exactly match DDGIUpdateIrradiance's projection basis.
    return sh[0] * 0.28209479f
         + sh[1] * (-0.48860251f * y)
         + sh[2] * ( 0.48860251f * z)
         + sh[3] * (-0.48860251f * x);
}

// 2026-09-05：移植 Vulkan route-A 的可见性/法线加权（DDGIGIGather.comp
// sampleNearProbeDepth + visibilityWeight）。此前 Metal gather 无可见性
// 测试——墙后探针漏光，跨平台对比偏亮且 GI 出现在不该亮的区域。
float2 gatherOctEncode(float3 d) {
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 u = d.xy / l1norm;
    float2 s = float2(u.x >= 0.0f ? 1.0f : -1.0f, u.y >= 0.0f ? 1.0f : -1.0f);
    if (d.z < 0.0f) u = (1.0f - abs(float2(u.y, u.x))) * s;
    return u * 0.5f + 0.5f;
}

float2 sampleNearProbeDepth(uint probeIdx, float3 dir, device const float* depthBuf) {
    float2 uv = gatherOctEncode(dir);
    uint2 texel = uint2(clamp(uint(uv.x * float(DEPTH_RES)), 0u, uint(DEPTH_RES - 1u)),
                        clamp(uint(uv.y * float(DEPTH_RES)), 0u, uint(DEPTH_RES - 1u)));
    uint octIdx = texel.y * uint(DEPTH_RES) + texel.x;
    uint base = probeIdx * DEPTH_FLOATS_PER_PROBE;
    return float2(depthBuf[base + octIdx], depthBuf[base + uint(DEPTH_TEXELS) + octIdx]);
}

float visibilityWeight(float distToProbe, float mean, float variance, float spacing) {
    float dist = max(distToProbe - mean, 0.0f);
    if (variance < 0.0001f) return 1.0f;
    float cheb = variance / (variance + dist * dist);
    return clamp(max(cheb * 2.0f, 0.2f), 0.0f, 1.0f);
}

kernel void ddgi_gi_gather(
    uint2 tid [[thread_position_in_grid]],
    depth2d<float, access::sample> depthTex [[texture(0)]],
    texture2d<float, access::sample> normalTex [[texture(1)]],
    texture2d<float, access::read_write> outputTex [[texture(2)]],
    texture2d<float, access::sample> historyTex [[texture(3)]],   // unused (Vulkan legacy: temporal disabled)
    texture2d<float, access::sample> ormTex [[texture(4)]],
    texture2d<float, access::write> specularTex [[texture(5)]],

    constant float4* viewCB                [[buffer(0)]],
    constant float4& probeOriginSpacing    [[buffer(1)]],
    constant float4& probeCountsCB         [[buffer(2)]],
    device const float3* staticSkySHBuffer [[buffer(3)]],         // unused (Vulkan legacy route)
    device const float* staticSkyFactor    [[buffer(4)]],         // unused
    constant StaticProbeData& staticProbe  [[buffer(5)]],         // unused
    device const float* confidenceBuffer   [[buffer(6)]],         // unused
    device const float3* irradianceBuffer  [[buffer(7)]],
    device const float* ddgiDepthBuffer    [[buffer(8)]]
) {
    uint width = outputTex.get_width();
    uint height = outputTex.get_height();

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

    uint3 counts = uint3(uint(probeCountsCB.x), uint(probeCountsCB.y), uint(probeCountsCB.z));

    // ========================================================================
    // Phase 1: per-pixel output (Vulkan legacy route)
    // ========================================================================
    if (inBounds && !active) {
        // Outside the near grid (or sky): dim ambient (legacy parity).
        outputTex.write(float4(0.03f, 0.03f, 0.035f, 1.0f), tid);
        specularTex.write(float4(0.0f), tid);
    }

    if (active) {
        // Select the trilinear cell per pixel. The previous cooperative path
        // selected one representative cell for the whole 8x8 screen tile;
        // pixels crossing a world-space probe boundary then combined their own
        // fractional weights with another pixel's probe indices, producing the
        // visible jagged checker/grid seams.
        uint3 b0 = uint3(clamp(floor(gp), float3(0.0f), float3(counts - 1u)));
        uint3 b1 = min(b0 + 1u, counts - 1u);
        uint3 pc[8];
        pc[0] = uint3(b0.x, b0.y, b0.z); pc[1] = uint3(b1.x, b0.y, b0.z);
        pc[2] = uint3(b0.x, b1.y, b0.z); pc[3] = uint3(b1.x, b1.y, b0.z);
        pc[4] = uint3(b0.x, b0.y, b1.z); pc[5] = uint3(b1.x, b0.y, b1.z);
        pc[6] = uint3(b0.x, b1.y, b1.z); pc[7] = uint3(b1.x, b1.y, b1.z);

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

        float3 result = float3(0.0f);
        float totalW = 0.0f;

        // T4.3 glossy: reflection-vector SH9 with the same plain trilinear
        // weights; roughness widens the lobe towards the diffuse limit.
        float3 camPos = viewCB[4].xyz;
        float3 V = normalize(camPos - worldPos);
        float NdotV = clamp(dot(normal, V), 0.05f, 1.0f);
        float3 R = reflect(-V, normal);
        float roughness = clamp(ormTex.sample(sampler(address::clamp_to_edge), uv).g, 0.04f, 1.0f);

        float3 reflResult = float3(0.0f);

        // 可见性/法线加权的低频保底分量（burial 掩埋探针不参与）——
        // 细薄几何（布料/栏杆）否则会把携带彩色反弹的邻居探针拒掉塌成黑。
        float3 lowFrequencyGI = float3(0.0f);
        float lowFrequencyW = 0.0f;
        float bestBaseWeight = 0.0f;
        float3 bestIrr = float3(0.0f);
        // 探针选择偏置：沿法线推 5% 间距，避免表面像素选中墙内探针。
        float3 biasedPos = worldPos + normal * probeOriginSpacing.w * 0.05f;

        for (uint p = 0u; p < 8u; ++p) {
            if (tw[p] < 0.01f) continue;
            uint pIdx = pc[p].x + pc[p].y * counts.x + pc[p].z * counts.x * counts.y;
            uint shBase = pIdx * SH_COEFF_COUNT;
            float3 sh[4];
            for (uint i = 0u; i < 4u; ++i)
                sh[i] = irradianceBuffer[shBase + i];

            // Proper SH evaluation at the pixel's surface normal.
            float3 irr = evalSH4Canonical(sh, normal);

            float w = tw[p];
            float insidePenalty = 1.0f;
            float burial = 1.0f;
            {
                float3 probePos = probeOriginSpacing.xyz + float3(pc[p]) * probeOriginSpacing.w;
                float3 toProbeVec = probePos - biasedPos;
                float distToProbe = length(toProbeVec);
                float3 toProbe = toProbeVec / max(distToProbe, 1e-5f);
                // 法线项连续保留（0.2 底）——全拒会在薄/双面几何处产生
                // 探针尺寸的未点亮格。
                float normalWeight = mix(0.2f, 1.0f,
                    clamp((dot(normal, toProbe) + 0.5f) / 0.7f, 0.0f, 1.0f));
                float2 depthMV = sampleNearProbeDepth(pIdx, -toProbe, ddgiDepthBuffer);
                if (depthMV.x < probeOriginSpacing.w * 0.2f) {
                    insidePenalty = mix(0.2f, 1.0f,
                        smoothstep(0.0f, 0.2f, depthMV.x / probeOriginSpacing.w));
                    burial = mix(0.1f, 1.0f,
                        smoothstep(0.0f, 0.2f, depthMV.x / probeOriginSpacing.w));
                }
                float nwSoft = max(normalWeight, 0.40f);
                float vw = max(visibilityWeight(distToProbe, depthMV.x, depthMV.y,
                                                probeOriginSpacing.w), 0.38f);
                w *= nwSoft * vw * insidePenalty;
            }

            lowFrequencyGI += irr * tw[p] * burial;
            lowFrequencyW += tw[p] * burial;
            if (tw[p] > bestBaseWeight) { bestBaseWeight = tw[p]; bestIrr = irr; }

            result += irr * w;
            totalW += w;

            // Reuse the same four loads for the reflection direction.
            reflResult += evalSH4Canonical(sh, R) * w;
        }
        if (totalW > 0.0f) {
            result /= totalW;
            reflResult /= totalW;
        } else {
            // 深度数据异常时的防御回退：正常路径权重连续非零。
            result = bestIrr;
        }
        if (lowFrequencyW > 0.0f) {
            lowFrequencyGI /= lowFrequencyW;
            // 45% 可见性抗拒漏光 + 55% 低频保底（细薄几何的探针被
            // Chebyshev 拒掉时近距反弹仍保留）。
            result = mix(lowFrequencyGI, result, 0.45f);
        }

        // Radiance-unit base for the specular blend (pre-correction scale);
        // the diffuse output gets the radiance→irradiance factor instead.
        float3 radBase = max(result, float3(0.05f, 0.05f, 0.06f));  // sky-leak ambient floor

        // Glossy output: RGB = lobe radiance, A = NdotV (DFG in FusionComposite).
        float3 specIrr = mix(reflResult, radBase, roughness * roughness);
        specularTex.write(float4(specIrr, NdotV), tid);

        float3 diffOut = max(radBase * PROBE_SH_RAD_TO_IRRAD, float3(0.05f, 0.05f, 0.06f));

        // Vulkan legacy parity: the probe/SC chain owns temporal convergence.
        // An additional screen-space EMA masks the triple-buffer phase bug and
        // ghosts during camera motion, so publish the gathered value directly.
        outputTex.write(float4(diffOut, 1.0f), tid);
    }

    (void)staticSkySHBuffer;
    (void)staticSkyFactor;
    (void)staticProbe;
    (void)confidenceBuffer;
    (void)historyTex;
}
