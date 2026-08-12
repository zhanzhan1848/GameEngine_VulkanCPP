#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::volume {

// ============================================================================
// VolumeParams — GPU constant buffer struct (must match Metal shader)
// ============================================================================

struct VolumeParams {
    // Volume bounds
    math::v4 VolumeOrigin;          // 0:   xyz = origin, w unused
    math::v4 VolumeExtent;          // 16:  xyz = extent, w unused

    // Camera
    math::v4 CameraPos;             // 32:  xyz = camera position, w unused
    math::v4 InvViewProj[4];        // 48:  inverse view-projection matrix (64 bytes)

    // Ray march parameters
    float    StepSize;              // 112
    float    MaxDistance;           // 116
    float    ExtinctionScale;       // 120
    float    ScatteringAlbedo;      // 124
    float    DensityThreshold;      // 128
    float    DensityFadeRange;      // 132
    u32      MaxSteps;              // 136
    u32      FrameIndex;            // 140

    // Screen dimensions
    u32      ScreenWidth;           // 144
    u32      ScreenHeight;          // 148
    float    _pad0[2];             // 152:  align to 160

    // GlobalSDF cascade data (same layout as DDGIVolumeData)
    math::v4 SdfOrigins[3];        // 160
    math::v4 SdfVoxelSizes[3];     // 208
    math::v4 SdfExtents[3];        // 256
    u32      SdfResolutions[3];    // 304
    u32      SdfCascadeCount;      // 316
    float    _pad1[1];             // 320:  align to 324

    // Light
    math::v4 LightDirection;       // 336:  xyz = direction, w unused
    math::v4 LightColor;           // 352:  xyz = color, w unused
};

static_assert(sizeof(VolumeParams) == 368, "VolumeParams GPU layout mismatch");

// ============================================================================
// Runtime configuration
// ============================================================================

struct VolumeRuntimeParams {
    float step_size          = 2.0f;
    float max_distance       = 200.0f;
    float extinction_scale   = 0.1f;
    float scattering_albedo  = 0.8f;
    u32   max_steps          = 64;
    float density_threshold  = 0.01f;
    float density_fade_range = 10.0f;
};

// ============================================================================
// Per-frame camera/light data provided by the caller
// ============================================================================

struct VolumeCameraData {
    math::v3   camera_position;
    math::m4x4 view_matrix;
    math::m4x4 proj_matrix;
    math::v3   light_direction;
    math::v3   light_color;
    u32        frame_index;
};

} // namespace primal::graphics::volume
