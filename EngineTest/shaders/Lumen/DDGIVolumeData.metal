/**
 * @file DDGIVolumeData.metal
 * @brief Shared DDGI volume parameters, data structures, and helpers.
 *
 * This is the single source of truth for DDGI shader data structures.
 * All DDGI shaders include this file.
 *
 * IMPORTANT: The struct layout must match the C++ DDGIVolumeData exactly.
 * C++ uses math::v4 (16 bytes, 16-byte aligned) for all vector types.
 * Metal uses float4 (same layout). float3/uint3 have different size/alignment
 * than C++ v4/v3u — so we use float4/uint[4] with explicit padding to match.
 */

#ifndef DDGI_VOLUME_DATA_METAL
#define DDGI_VOLUME_DATA_METAL

// ============================================================================
// Constants
// ============================================================================

#define DDGI_SH_COEFF_COUNT  9
#define DDGI_DEPTH_RES       8
#define DDGI_DEPTH_TEXELS    64  // DDGI_DEPTH_RES * DDGI_DEPTH_RES
#define DDGI_MAX_SDF_STEPS   4
#define DDGI_SH_C0           0.282095f
#define DDGI_SH_C1           0.488603f
#define DDGI_SKY_COLOR       float3(0.3f, 0.3f, 0.35f)
constant float DDGI_SH_C2_0  = 1.092548f;
constant float DDGI_SH_C2_1  = 0.315392f;
constant float DDGI_SH_C2_2  = 0.546274f;

// ============================================================================
// DDGIVolumeData (constant buffer, ~304 bytes)
// Must match C++ DDGIVolumeData struct in LumenDDGIPass.cpp EXACTLY.
//
// C++ uses math::v4 (float[4], align 16) for ProbeOrigin, SdfOrigins, etc.
// Metal float3 has size 12 but C++ v4 has size 16 — so we use float4 here
// to match. Similarly uint3 (Metal: size 12) vs u32[4] (C++: size 16).
// ============================================================================

struct DDGIVolumeData {
    // Probe grid definition
    float4   ProbeOrigin;             //   0: xyz = origin, w unused (C++: math::v4)
    float    ProbeSpacing;            //  16

    float    _pad_before_counts[3];   //  20: matches C++ explicit padding
    uint     ProbeCounts[4];          //  32: [0]=Nx [1]=Ny [2]=Nz [3] unused
    uint     RaysPerProbe;            //  48
    uint     ProbeCountTotal;         //  52

    // Temporal filtering
    float    IrradianceBlurSigma;     //  56
    float    DepthBlurSigma;          //  60

    // Frame
    float    DeltaTime;               //  64
    uint     FrameIndex;              //  68
    float    RayMaxDistance;          //  72

    float    ProbeHysteresis;         //  76
    float    TemporalAlpha;           //  80
    uint     ProbeUpdateCount;        //  84

    uint     SCLookupCount;           //  88 — Surface Cache card lookup count
    uint     SCAtlasSize;             //  92 — Surface Cache lighting atlas size
    int      ProbeRelocationShift[3]; //  96: grid shift in probe cells
    // T8.13 sync: probe ray sampler mode (Vulkan UBO offset 108). 0 = legacy
    // fixed spiral, 1 = per-frame golden-angle jitter, 2 = cosine mixture.
    // Semantics: ProbeSampleLib (ddgiSampleRayDirection below).
    int      RaySampleMode;           // 108

    // GlobalSDF cascade data (3 cascades)
    float4   SdfOrigins[3];           // 112
    float4   SdfVoxelSizes[3];        // 160
    float4   SdfExtents[3];           // 208
    uint     SdfResolutions[3];       // 256
    uint     SdfCascadeCount;         // 268

    float4   LightDirection;          // 272: xyz = light dir, w unused (C++: math::v4)
    float4   LightColor;              // 288: xyz = light color, w unused (C++: math::v4)

    // Mode 11 (MeshletDynamicDDGI) canonical radiance inputs — mirror of the
    // C++ tail (offsets 304/320). Struct grows 304→336B, byte-matched.
    float4   SkyColor;                // 304: xyz = sky color, w unused
    float4   Albedo;                  // 320: xyz = uniform albedo, w unused
};

// ============================================================================
// DDGIRayData (storage buffer element)
// Layout: ray_data[probeIdx * RaysPerProbe + rayIdx]
// ============================================================================

struct DDGIRayData {
    float4 radiance_and_dist;  // xyz = radiance, w = hit_distance
};

// ============================================================================
// SH3 evaluation
// ============================================================================

static void shEvaluate(float3 d, thread float* out)
{
    float x = d.x, y = d.y, z = d.z;
    float x2 = x * x, y2 = y * y, z2 = z * z;
    out[0] = DDGI_SH_C0;
    out[1] = -DDGI_SH_C1 * y;
    out[2] =  DDGI_SH_C1 * z;
    out[3] = -DDGI_SH_C1 * x;
    out[4] =  DDGI_SH_C2_0 * y * x;
    out[5] = -DDGI_SH_C2_0 * y * z;
    out[6] =  DDGI_SH_C2_1 * (3.0f * z2 - 1.0f);
    out[7] = -DDGI_SH_C2_0 * x * z;
    out[8] =  DDGI_SH_C2_2 * (x2 - y2);
}

static float3 shDot(thread const float3* shCoeffs, float3 d)
{
    float basis[9];
    shEvaluate(d, basis);
    float3 result = float3(0.0f);
    for (uint i = 0; i < 4u; ++i) { // Only L0+L1 for stability with 64 rays
        result += shCoeffs[i] * basis[i];
    }
    return result;
}

// ============================================================================
// Helpers
// ============================================================================

// Extract uint3 probe counts from the uint[4] array
static uint3 ddgiGetProbeCounts(constant DDGIVolumeData& vol) {
    return uint3(vol.ProbeCounts[0], vol.ProbeCounts[1], vol.ProbeCounts[2]);
}

static float3 ddgiRayDirection(uint rayIndex, uint rayCount, uint frameIndex)
{
    const float INV_PHI = 0.6180339887498948482f;
    const float PI = 3.14159265358979323846f;
    // Fixed directions — no frame rotation.
    // Rotating ray directions each frame destabilizes SH projection with 64 rays,
    // causing flickering. Fixed directions give stable per-probe results that
    // converge smoothly via temporal EMA.
    float u = fract((float(rayIndex) + 0.5f) * INV_PHI);
    float v = fract((float(rayIndex) + 0.5f) * INV_PHI * INV_PHI);
    float theta = 2.0f * PI * u;
    float phi   = acos(1.0f - 2.0f * v);
    float sinPhi = sin(phi);
    return float3(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi));
}

// ============================================================================
// Probe ray sampler — Metal port of Vulkan ShaderLib/ProbeSampleLib.glsl
// (T8.13 sync). Single source for the trace side AND the SH projection: the
// projection re-derives directions from ray indices, so every direction change
// must go through this function or SH desynchronizes from traced radiance.
// Modes (vol.RaySampleMode, UBO offset 108; DDGI_RAY_SAMPLE env):
//   0 UNIFORM — legacy fixed spiral, bit-identical to ddgiRayDirection.
//   1 JITTER  — golden-angle rotation per frame.
//   2 COSINE  — 50% cosine lobe around the light + 50% uniform mixture.
//   3 STRATIFIED — fixed golden-angle azimuth + stratified latitude.
//   4 DEPTH_FILL — same directions/pdf as mode 0 (Vulkan-only depth diagnostic).
// Returns float4(dir, pdf).
// ============================================================================
static float4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex,
                                     uint mode, float3 toLight)
{
    const float INV_PHI = 0.6180339887498948482f;
    const float TWO_PI  = 6.28318530717958647692f;

    float u = fract((float(rayIndex) + 0.5f) * INV_PHI);
    float v = fract((float(rayIndex) + 0.5f) * INV_PHI * INV_PHI);

    if (mode != 0u && mode != 3u && mode != 4u) {
        u = fract(u + float(frameIndex) * INV_PHI);
        v = fract(v + float(frameIndex) * INV_PHI * INV_PHI);
    }

    if (mode == 3u)
        v = (float(rayIndex) + 0.5f) / max(float(rayCount), 1.0f);

    if (mode == 2u) {
        float3 w = normalize(toLight);
        // Minimal-rotation ONB from +Z to w (Duff et al. 2017).
        float s = w.z >= 0.0f ? 1.0f : -1.0f;
        float a = -1.0f / (s + w.z);
        float b = w.x * w.y * a;
        float3 tangent   = float3(1.0f + s * w.x * w.x * a, s * b, -s * w.x);
        float3 bitangent = float3(b, s + w.y * w.y * a, -w.y);

        float3 dir;
        if (v < 0.5f) {
            float xi   = v * 2.0f;
            float cosT = sqrt(max(1.0f - xi, 0.0f));
            float sinT = sqrt(max(1.0f - cosT * cosT, 0.0f));
            float th   = TWO_PI * u;
            dir = w * cosT + tangent * (sinT * cos(th)) + bitangent * (sinT * sin(th));
        } else {
            float vu  = (v - 0.5f) * 2.0f;
            float ph  = acos(clamp(1.0f - 2.0f * vu, -1.0f, 1.0f));
            float sp  = sin(ph);
            float th  = TWO_PI * u;
            dir = float3(sp * cos(th), sp * sin(th), cos(ph));
        }
        float cosL = max(dot(dir, w), 0.0f);
        float pdf = max(0.5f * cosL / M_PI_F + 0.5f / (4.0f * M_PI_F), 1e-5f);
        return float4(dir, pdf);
    }

    // Modes 0, 1, 3 and 4: uniform on the sphere, pdf = 1/(4π).
    float theta = TWO_PI * u;
    float phi   = acos(clamp(1.0f - 2.0f * v, -1.0f, 1.0f));
    float sinPhi = sin(phi);
    return float4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi),
                  0.25f / M_PI_F);
}

static uint3 ddgiProbeGridCoord(uint probeIdx, uint3 counts)
{
    uint pz = probeIdx / (counts.x * counts.y);
    uint rem = probeIdx % (counts.x * counts.y);
    uint py = rem / counts.x;
    uint px = rem % counts.x;
    return uint3(px, py, pz);
}

static float3 ddgiProbeWorldPos(uint3 gc, float3 origin, float spacing)
{
    return origin + float3(float(gc.x), float(gc.y), float(gc.z)) * spacing;
}

// ============================================================================
// Octahedral depth mapping
// ============================================================================

// Sphere direction → octahedral UV in [0,1]²
static float2 octahedralEncode(float3 d)
{
    float l1norm = abs(d.x) + abs(d.y) + abs(d.z);
    float2 uv = d.xy / l1norm;
    if (d.z < 0.0f) {
        uv = (1.0f - abs(uv.yx)) * select(float2(-1.0f), float2(1.0f), uv.xy >= 0.0f);
    }
    return uv * 0.5f + 0.5f;
}

// Octahedral UV in [0,1]² → sphere direction
static float3 octahedralDecode(float2 uv)
{
    float2 p = uv * 2.0f - 1.0f;
    float3 d = float3(p, 1.0f - abs(p.x) - abs(p.y));
    if (d.z < 0.0f) {
        d.xy = (1.0f - abs(d.yx)) * select(float2(-1.0f), float2(1.0f), d.xy >= 0.0f);
    }
    return normalize(d);
}

// Nearest-neighbor depth sampling from octahedral storage buffer.
// Conservative: avoids bilinear blending across depth discontinuities
// which creates false mid-depths and causes Chebyshev visibility failures.
// Returns float2(mean, variance)
static float2 sampleDepthOctahedral(device const float* buf, uint probeIdx, float3 dir)
{
    constexpr uint res = DDGI_DEPTH_RES;
    constexpr uint texels = res * res;

    float2 uv = octahedralEncode(dir);
    uint2 texel = uint2(clamp(uint(uv.x * float(res)), 0u, res - 1u),
                        clamp(uint(uv.y * float(res)), 0u, res - 1u));
    uint idx = texel.y * res + texel.x;
    uint probeBase = probeIdx * texels * 2u;
    return float2(buf[probeBase + idx], buf[probeBase + texels + idx]);
}

#endif // DDGI_VOLUME_DATA_METAL
