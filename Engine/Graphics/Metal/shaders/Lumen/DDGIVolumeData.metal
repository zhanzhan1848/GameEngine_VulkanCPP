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

#define DDGI_SH_COEFF_COUNT  4
#define DDGI_DEPTH_DIRS      8
#define DDGI_MAX_SDF_STEPS   128
#define DDGI_SH_C0           0.282095f
#define DDGI_SH_C1           0.488603f
#define DDGI_SKY_COLOR       float3(0.05f, 0.05f, 0.08f)

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

    float    _pad0;                // 56
    float    _pad1;                // 60

    // GlobalSDF cascade data (3 cascades)
    float4   SdfOrigins[3];       //  64  (float3 + 4 bytes padding)
    float4   SdfVoxelSizes[3];    // 80 (float + 12 bytes padding)
    float4   SdfExtents[3];       // 112 (float3 + 4 bytes padding)
    uint     SdfResolutions[3];   // 148
    uint     SdfCascadeCount;     // 160
    float    _pad2[3];            // 164
};

// ============================================================================
// DDGIRayData (storage buffer element)
// Layout: ray_data[probeIdx * RaysPerProbe + rayIdx]
// ============================================================================

struct DDGIRayData {
    float4 radiance_and_dist;  // xyz = radiance, w = hit_distance
};

// ============================================================================
// Helpers
// ============================================================================

static float3 ddgiFibonacciSphereDir(uint rayIndex, uint rayCount, uint frameIndex)
{
    const float GOLDEN_ANGLE = 2.39996323f;
    float phi = GOLDEN_ANGLE * float(rayIndex) + float(frameIndex) * 0.618033988749f;
    float cosTheta = 1.0f - 2.0f * (float(rayIndex) + 0.5f) / float(rayCount);
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
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
