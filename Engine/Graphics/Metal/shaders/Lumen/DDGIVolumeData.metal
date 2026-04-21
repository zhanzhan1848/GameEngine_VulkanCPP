/**
 * @file DDGIVolumeData.metal
 * @brief Shared DDGI volume parameters, data structures, and helpers.
 *
 * This is the single source of truth for DDGI shader data structures.
 * All DDGI shaders include this file.
 *
 * The C++ struct layout must match exactly — see LumenDDGIPass.cpp for the
 * matching C++ definitions used when uploading constant buffers.
 */

#ifndef DDGI_VOLUME_DATA_METAL
#define DDGI_VOLUME_DATA_METAL

// ============================================================================
// Constants
// ============================================================================

#define DDGI_SH_COEFF_COUNT  9
#define DDGI_DEPTH_DIRS      8
#define DDGI_MAX_SDF_STEPS   128
#define DDGI_SH_C0           0.282095f
#define DDGI_SH_C1           0.488603f
#define DDGI_SKY_COLOR       float3(0.05f, 0.05f, 0.08f)
constant float DDGI_SH_C2_0  = 1.092548f;
constant float DDGI_SH_C2_1  = 0.315392f;
constant float DDGI_SH_C2_2  = 0.546274f;

// ============================================================================
// DDGIVolumeData (constant buffer, ~256 bytes)
// Must match C++ DDGIVolumeData struct in LumenDDGIPass.cpp
// ============================================================================

struct DDGIVolumeData {
    // Probe grid definition
    float3   ProbeOrigin;          //  0: World-space origin of the grid
    float    ProbeSpacing;         // 12: Distance between probes
    uint3    ProbeCounts;          // 16: (Nx, Ny, Nz)
    uint     RaysPerProbe;         // 28: 128
    uint     ProbeCountTotal;      // 32: Nx*Ny*Nz

    // Temporal filtering
    float    IrradianceBlurSigma;  // 36: EMA alpha for irradiance (0.02)
    float    DepthBlurSigma;       // 40: EMA alpha for depth (0.2)

    // Frame
    float    DeltaTime;            // 44
    uint     FrameIndex;           // 48
    float    RayMaxDistance;       // 52: 20.0

    float    ProbeHysteresis;      // 56
    float    TemporalAlpha;        // 60

    // GlobalSDF cascade data (3 cascades)
    float4   SdfOrigins[3];       //  64  (float3 + 4 bytes padding)
    float4   SdfVoxelSizes[3];    // 80 (float + 12 bytes padding)
    float4   SdfExtents[3];       // 112 (float3 + 4 bytes padding)
    uint     SdfResolutions[3];   // 148
    uint     SdfCascadeCount;     // 160
    float3   LightDirection;      // 256: normalized light direction (world space)
    float3   LightColor;          // 272: light color (linear HDR)
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

static float3 ddgiRayDirection(uint rayIndex, uint rayCount, uint frameIndex)
{
    const float INV_PHI = 0.6180339887498948482f;
    const float PI = 3.14159265358979323846f;
    // Fixed R2 directions (no per-frame rotation) — eliminates L1 temporal noise
    // with 64 rays. Rotation causes frame-to-frame L1 variance that tetrahedral
    // interpolation amplifies into visible flickering, even with hysteresis 0.01.
    float u = fract((float(rayIndex) + 0.5f) * INV_PHI);
    float v = fract((float(rayIndex) + 0.5f) * INV_PHI * INV_PHI);
    float theta = 2.0f * PI * u;
    float phi   = acos(1.0f - 2.0f * v);
    float sinPhi = sin(phi);
    return float3(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi));
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

#endif // DDGI_VOLUME_DATA_METAL
