#ifndef SDF_TRACE_COMMON_METAL
#define SDF_TRACE_COMMON_METAL

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Constants
// ============================================================================

#ifndef SDF_MAX_STEPS
#define SDF_MAX_STEPS 4
#endif

constant float SDF_RELAXATION_OMEGA   = 1.2f;
constant float SDF_HIT_THRESHOLD_FACTOR = 0.5f;   // voxelSize * this
constant float SDF_MIN_STEP_FACTOR    = 0.25f;     // voxelSize * this
constant float SDF_RELAXATION_GUARD   = 4.0f;      // relax when d > threshold * this

// ============================================================================
// SDF Hit Result
// ============================================================================

struct SDFHitResult {
    bool   hit;
    float3 position;
    float  distance;
};

// ============================================================================
// Cascade helpers
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
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    return sdfTexture.sample(s, uvw).r;
}

// ============================================================================
// else-if cascade selection — 1 texture3D read per call (Apple Silicon safe)
// Finest cascade first for best accuracy.
// ============================================================================

static float sampleBestSDF_elseIf(float3 pos,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    float4 origin0, float4 origin1, float4 origin2,
    float4 extent0, float4 extent1, float4 extent2,
    uint cascadeCount)
{
    if (cascadeCount > 0 &&
        isInsideCascade(pos, origin0.xyz, extent0.xyz))
        return sampleSDFCascade(sdf0, pos, origin0.xyz, extent0.xyz);
    else if (cascadeCount > 1 &&
        isInsideCascade(pos, origin1.xyz, extent1.xyz))
        return sampleSDFCascade(sdf1, pos, origin1.xyz, extent1.xyz);
    else if (cascadeCount > 2 &&
        isInsideCascade(pos, origin2.xyz, extent2.xyz))
        return sampleSDFCascade(sdf2, pos, origin2.xyz, extent2.xyz);
    return 1e10f;
}

// ============================================================================
// Relaxed sphere tracing — 4 steps, omega=1.2, adaptive threshold
// ============================================================================

static SDFHitResult traceSDF_relaxed(
    float3 rayOrigin,
    float3 rayDir,
    float  maxDist,
    float  finestVoxelSize,
    float  fallbackStride,
    texture3d<float, access::sample> sdf0,
    texture3d<float, access::sample> sdf1,
    texture3d<float, access::sample> sdf2,
    float4 origin0, float4 origin1, float4 origin2,
    float4 extent0, float4 extent1, float4 extent2,
    uint   cascadeCount)
{
    SDFHitResult result;
    result.hit = false;
    result.position = rayOrigin;
    result.distance = maxDist;

    float hitThreshold = finestVoxelSize * SDF_HIT_THRESHOLD_FACTOR;
    float minStep = finestVoxelSize * SDF_MIN_STEP_FACTOR;
    float t = 0.0f;
    float prevD = 3.4e38f;   // B13: distance trend across steps

    for (uint step = 0; step < SDF_MAX_STEPS; ++step) {
        float3 pos = rayOrigin + rayDir * t;

        // else-if cascade selection — 1 texture3D read
        float d = sampleBestSDF_elseIf(pos, sdf0, sdf1, sdf2,
                                       origin0, origin1, origin2,
                                       extent0, extent1, extent2,
                                       cascadeCount);

        // Outside all cascades — use fallback stride
        if (d >= 1e9f) {
            t += fallbackStride;
            if (t > maxDist) break;
            continue;
        }

        // Hit detection
        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = t;
            break;
        }

        // Relaxed sphere tracing: overstep when far, conservative when close.
        // B13: only over-relax while the field is NOT shrinking — an ω step
        // onto a thin wall (thickness < ~2.5 voxels) lands past it and the
        // unsigned field never reports the crossing.
        float advance;
        if (d > hitThreshold * SDF_RELAXATION_GUARD && d >= prevD) {
            advance = d * SDF_RELAXATION_OMEGA;
        } else {
            advance = max(d, minStep);
        }
        prevD = d;
        t += advance;
        if (t > maxDist) break;
    }

    return result;
}

#endif // SDF_TRACE_COMMON_METAL

// ============================================================================
// T3.2 (D7): cone-width-aware cascade sampling primitives (Vulkan twin in
// ShaderLib/TraceLib.glsl). Project convention: texel-read manual trilinear
// (R32F is not filterable). mipTex = ONE texture from the cascade's
// standalone min-mip chain (DispatchMipChain); get_width() gives its dims.
// sampleSDFCascadeMip — trilinear within one mip level (conservative MIN).
// coneMipFrac         — footprint/voxelSize → (level, frac) across two mips.
// Both return LOWER-bound distances; cone hit tests are conservative.
// ============================================================================
static float sampleSDFCascadeMip(texture3d<float, access::read> mipTex,
                                 float3 worldPos,
                                 float3 cascadeOrigin, float3 cascadeExtent) {
    float3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (uvw.x < 0.0f || uvw.x > 1.0f ||
        uvw.y < 0.0f || uvw.y > 1.0f ||
        uvw.z < 0.0f || uvw.z > 1.0f) return 1e10f;
    uint3 size = uint3(mipTex.get_width(), mipTex.get_height(), mipTex.get_depth());
    float3 p = clamp(uvw, float3(0.0f), float3(1.0f)) * float3(size) - 0.5f;
    uint3 base = uint3(clamp(floor(p), float3(0.0f), float3(size) - 1.0f));
    float3 f = fract(p);
    uint3 c1 = min(base + 1u, size - 1u);
    float d000 = mipTex.read(uint3(base.x, base.y, base.z)).r;
    float d100 = mipTex.read(uint3(c1.x,  base.y, base.z)).r;
    float d010 = mipTex.read(uint3(base.x, c1.y,  base.z)).r;
    float d110 = mipTex.read(uint3(c1.x,  c1.y,  base.z)).r;
    float d001 = mipTex.read(uint3(base.x, base.y, c1.z)).r;
    float d101 = mipTex.read(uint3(c1.x,  base.y, c1.z)).r;
    float d011 = mipTex.read(uint3(base.x, c1.y,  c1.z)).r;
    float d111 = mipTex.read(uint3(c1.x,  c1.y,  c1.z)).r;
    return mix(mix(mix(d000, d100, f.x), mix(d010, d110, f.x), f.y),
               mix(mix(d001, d101, f.x), mix(d011, d111, f.x), f.y), f.z);
}

// footprint → (level, frac) — see Vulkan twin.
static float2 coneMipFrac(float footprint, float voxelSize, uint maxMip) {
    float t = clamp(log2(max(footprint / (voxelSize * 2.0f), 1.0f)), 0.0f, float(maxMip) - 1.0f);
    float level = floor(t);
    return float2(level, t - level);
}
