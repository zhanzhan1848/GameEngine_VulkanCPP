#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Utilities/MathTypes.h"

namespace primal::graphics::fluid {

// ============================================================================
// FluidParams — GPU constant buffer (must match Metal shader)
// ============================================================================
// Layout: 224 bytes. Allocated as 256 bytes in constant buffer.

struct FluidParams {
    // Camera (16 bytes)
    math::v4 CameraPos;          // 0: xyz = camera position

    // Screen (16 bytes)
    math::v4 ScreenParams;       // 16: x=width, y=height, z=1/width, w=1/height

    // Inverse view-projection (64 bytes)
    math::v4 InvViewProj[4];     // 32

    // View-projection (64 bytes)
    math::v4 ViewProj[4];        // 96

    // Fluid parameters (48 bytes)
    math::v4 FluidParams1;       // 160: x=particle_radius, y=splat_scale, z=absorption, w=ior
    math::v4 FluidParams2;       // 176: x=smooth_sigma_depth, y=smooth_iterations, z=light_dir_x, w=light_dir_y
    math::v4 FluidParams3;       // 192: x=light_dir_z, y=light_color_r, z=light_color_g, w=light_color_b

    // Depth (16 bytes)
    math::v4 DepthParams;        // 208: x=near, y=far
};

static_assert(sizeof(FluidParams) == 224, "FluidParams GPU layout mismatch");

// ============================================================================
// Fluid Configuration
// ============================================================================

struct FluidConfig {
    float particle_radius     = 0.1f;
    float splat_scale         = 1.5f;
    float absorption          = 2.0f;
    float ior                 = 1.33f;
    float smooth_sigma_depth  = 1.0f;
    u32   smooth_iterations   = 2;
};

} // namespace primal::graphics::fluid
