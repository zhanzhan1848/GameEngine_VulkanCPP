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

#ifndef DETAIL_SDF_COMMON_METAL
#define DETAIL_SDF_COMMON_METAL

// Mirror of LumenDDGIPass.h DetailSDFSlot table (std140-aligned UBO on
// Vulkan; constant buffer here). Decode contract:
//   signed (params.z=1): d = unorm * 2*max_dim - max_dim
//   legacy unsigned    : d = unorm * max_dim
// max_dim = padded bounds max extent / 1.4 (ContentTools uniform 20%
// padding); bounds_min/max are the WORLD AABB of the mesh SDF grid.
struct DetailSDFTableGPU {
    float4 header;               // x = active slot count (0..4)
    float4x4 inv_world[4];       // world → mesh-local
    float4 normal_matrix[4][3];  // rows of transpose(inverse(world 3x3))
    float4 bounds_min[4];
    float4 bounds_max[4];
    float4 params[4];            // resolution, max_dim, signed, voxel_size
};

static float detailSDFDecode(constant DetailSDFTableGPU& dt, uint k, float raw) {
    float max_dim = dt.params[k].y;
    if (dt.params[k].z > 0.5f) return raw * 2.0f * max_dim - max_dim;
    return raw * max_dim;
}

// Normalized-coords linear sample; the unorm decode is affine so it commutes
// with linear filtering (parity with the Vulkan 8-tap manual trilinear).
static float detailSDFSample(texture3d<float, access::sample> tex,
                              constant float4& bmin, constant float4& bmax,
                              float res, float3 local) {
    float3 uvw = (local - bmin.xyz) / max(bmax.xyz - bmin.xyz, float3(1e-6f));
    if (any(uvw < float3(0.0f)) || any(uvw > float3(1.0f))) return 1.0f;  // far
    constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
    return tex.sample(s, uvw).r;
}

// Refine the coarse GlobalSDF hit against the finest slot containing it.
// Returns true with refined t in outT and the gradient normal in outN
// (world space, oriented against the incident ray).
static bool detailSDFRefine(constant DetailSDFTableGPU& dt,
                             texture3d<float, access::sample> tex0,
                             texture3d<float, access::sample> tex1,
                             texture3d<float, access::sample> tex2,
                             texture3d<float, access::sample> tex3,
                             float3 probePos, float3 rayDir,
                             thread float& t, thread float3& outN) {
    outN = float3(0.0f);
    const uint count = uint(dt.header.x);
    if (count == 0u) return false;

    const float3 hitPos = probePos + rayDir * t;

    // Slot search: containing world AABB, smallest |d| at the hit (wall
    // seams put the point in several grids; the tightest match wins).
    int best = -1;
    float bestAbs = 3.4e38f;
    for (uint k = 0u; k < count; ++k) {
        if (any(hitPos < dt.bounds_min[k].xyz) || any(hitPos > dt.bounds_max[k].xyz)) continue;
        float3 local = (dt.inv_world[k] * float4(hitPos, 1.0f)).xyz;
        float d;
        if (k == 0u)      d = detailSDFDecode(dt, k, detailSDFSample(tex0, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else if (k == 1u) d = detailSDFDecode(dt, k, detailSDFSample(tex1, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else if (k == 2u) d = detailSDFDecode(dt, k, detailSDFSample(tex2, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        else              d = detailSDFDecode(dt, k, detailSDFSample(tex3, dt.bounds_min[k], dt.bounds_max[k], dt.params[k].x, local));
        if (fabs(d) < bestAbs) { bestAbs = fabs(d); best = int(k); }
    }
    if (best < 0) return false;
    const uint bk = uint(best);
    texture3d<float, access::sample> tex =
        (bk == 0u) ? tex0 : (bk == 1u) ? tex1 : (bk == 2u) ? tex2 : tex3;
    const float localVoxel = max(dt.params[bk].w, 1e-5f);
    const bool signedField = dt.params[bk].z > 0.5f;

    // Field value in t-parameter space; local(t) = inv_world * (p + r*t).
    #define DDGI_DETAIL_F(ti_) \
        detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], \
            dt.params[bk].x, (dt.inv_world[bk] * float4(probePos + rayDir * (ti_), 1.0f)).xyz))

    float fBest = bestAbs;
    float tBest = t;
    if (signedField) {
        // Signed field: scan a ±5*localVoxel window for the zero crossing,
        // then bisect (the coarse hit sits inside the inflated hit band).
        float tLo = t, tHi = t;
        bool haveLo = false, haveHi = false;
        for (int i = -5; i <= 5; ++i) {
            float ti = t + float(i) * localVoxel;
            float fi = DDGI_DETAIL_F(ti);
            if (fi < 0.0f && (!haveLo || ti > tLo)) { tLo = ti; haveLo = true; }
            if (fi >= 0.0f && (!haveHi || ti < tHi)) { tHi = ti; haveHi = true; }
            if (fabs(fi) < fBest) { fBest = fabs(fi); tBest = ti; }
        }
        if (haveLo && haveHi) {
            for (int it = 0; it < 5; ++it) {
                float tm = 0.5f * (tLo + tHi);
                if (DDGI_DETAIL_F(tm) < 0.0f) tLo = tm; else tHi = tm;
            }
            tBest = 0.5f * (tLo + tHi);
        }
    } else {
        // Legacy unsigned field: descend on |d| — the surface is the local
        // minimum of the distance band.
        for (int i = 1; i <= 6; ++i) {
            float ti = t + float(i) * 0.5f * localVoxel;
            float fi = DDGI_DETAIL_F(ti);
            if (fabs(fi) < fBest) { fBest = fabs(fi); tBest = ti; } else break;
        }
    }
    #undef DDGI_DETAIL_F

    // Gradient normal at the refined point (central difference, 6 taps).
    const float e = 0.75f * localVoxel;
    float3 lp = (dt.inv_world[bk] * float4(probePos + rayDir * tBest, 1.0f)).xyz;
    float dx = detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(e, 0, 0)))
             - detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(e, 0, 0)));
    float dy = detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(0, e, 0)))
             - detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(0, e, 0)));
    float dz = detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp + float3(0, 0, e)))
             - detailSDFDecode(dt, bk, detailSDFSample(tex, dt.bounds_min[bk], dt.bounds_max[bk], dt.params[bk].x, lp - float3(0, 0, e)));
    float3 gLocal = float3(dx, dy, dz);
    if (dot(gLocal, gLocal) < 1e-14f) return false;

    float3 n = normalize(
        dt.normal_matrix[bk][0].xyz * gLocal.x +
        dt.normal_matrix[bk][1].xyz * gLocal.y +
        dt.normal_matrix[bk][2].xyz * gLocal.z);
    if (dot(n, rayDir) > 0.0f) n = -n;   // orient against the incident ray
    outN = n;
    t = tBest;
    return true;
}



#endif // DETAIL_SDF_COMMON_METAL

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
