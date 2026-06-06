#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::volume {

// ============================================================================
// FroxelParams — GPU constant buffer struct (must match Metal shader)
// ============================================================================
// Layout: 384 bytes. Allocated as 512 bytes in constant buffer.

struct FroxelParams {
    // Camera (80 bytes)
    math::v4 CameraPos;           // 0:   xyz = camera position
    math::v4 InvViewProj[4];      // 16:  inverse view-projection matrix (64 bytes)

    // Frustum (16 bytes)
    math::v4 FrustumParams;       // 80:  x=near, y=far, z=log2(far/near), w=1.0/depth_slices

    // Froxel grid (32 bytes)
    math::v4 FroxelDims;          // 96:  x=width, y=height, z=depth, w=1.0/width
    math::v4 FroxelDims2;         // 112: x=1.0/height, y=1.0/depth, z=pad, w=pad

    // Volume params (48 bytes)
    math::v4 VolumeParams1;       // 128: x=extinction_scale, y=scattering_albedo, z=density_threshold, w=density_fade_range
    math::v4 VolumeParams2;       // 144: x=height_fog_base, y=height_fog_scale, z=ambient_intensity, w=time
    math::v4 VolumeParams3;       // 160: x=phase_g, y=noise_scale, z=noise_speed, w=pad

    // Screen (16 bytes)
    math::v4 ScreenParams;        // 176: x=screen_width, y=screen_height, z=1.0/width, w=1.0/height

    // GlobalSDF cascade data (same packing as VolumeParams)
    math::v4 SdfOrigins[3];       // 192
    math::v4 SdfVoxelSizes[3];    // 240
    math::v4 SdfExtents[3];       // 288
    math::v4 SdfResolutions;      // 336: xyz=resolutions, w=cascade_count

    // Light (32 bytes)
    math::v4 LightDirection;      // 352: xyz = direction
    math::v4 LightColor;          // 368: xyz = color

    // Shadow VP matrices (128 bytes)
    math::v4 ShadowVP[2][4];      // 384: [cascade][column], lightProj * lightView
};

static_assert(sizeof(FroxelParams) == 512, "FroxelParams GPU layout mismatch");

// ============================================================================
// Froxel Grid Configuration
// ============================================================================

struct FroxelGridConfig {
    u32   grid_width       = 160;
    u32   grid_height      = 90;
    u32   grid_depth       = 64;
    float near_plane       = 0.5f;
    float far_plane        = 200.0f;
    float extinction_scale  = 0.1f;
    float scattering_albedo = 0.8f;
    float density_threshold = 0.01f;
    float density_fade_range = 10.0f;
    float phase_g          = 0.0f;       // Henyey-Greenstein g parameter (0=isotropic)
    float noise_scale      = 1.0f;
    float noise_speed      = 0.1f;
    float height_fog_base  = 0.0f;
    float height_fog_scale = 0.0f;
    float ambient_intensity = 0.3f;
};

// Reuse VolumeCameraData from VolumeTypes.h for per-frame camera/light input.

} // namespace primal::graphics::volume
