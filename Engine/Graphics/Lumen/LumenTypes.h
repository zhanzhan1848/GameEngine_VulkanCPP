#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"

namespace primal::graphics::lumen {

/**
 * @brief Lumen GI quality presets
 */
enum class LumenQualityPreset {
    Off = 0,
    Low,       // SSGI only
    Medium,    // SSGI + DDGI
    High,      // SSGI + DDGI + Surface Cache
    Ultra,     // Screen Probes + Surface Cache + DDGI
    UltraRT    // Ultra + Hardware RT
};

/**
 * @brief Lumen configuration (runtime adjustable)
 */
struct LumenConfig {
    LumenQualityPreset quality = LumenQualityPreset::Medium;

    // SSGI settings
    u32 ssgi_ray_count = 4;
    float ssgi_radius = 2.0f;
    float ssgi_temporal_weight = 0.95f;

    // DDGI settings
    u32 ddgi_probe_count_x = 32;
    u32 ddgi_probe_count_y = 16;
    u32 ddgi_probe_count_z = 32;
    u32 ddgi_rays_per_probe = 128;
    float ddgi_probe_spacing = 1.0f;

    // Surface Cache settings
    u32 surface_cache_atlas_size = 4096;
    u32 surface_cache_page_size = 128;
    u32 surface_cache_capture_budget = 512;

    // Screen Probes settings
    u32 screen_probes_spacing = 8;
    u32 screen_probes_rays = 64;

    // Performance budget (ms)
    float max_gi_time_ms = 6.0f;
};

} // namespace primal::graphics::lumen
