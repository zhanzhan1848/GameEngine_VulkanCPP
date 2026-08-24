#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"

namespace primal::graphics::lumen {

enum class ShadowQuality { Hard = 0, PCF_16 = 1, PCSS = 2 };

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
    // DDGI probe grid — known-working config for Sponza.
    u32 ddgi_probe_count_x = 32;
    u32 ddgi_probe_count_y = 16;
    u32 ddgi_probe_count_z = 32;
    u32 ddgi_rays_per_probe = 64;    // T4.2 note: 128 blocked — ddgi_trace_sdf/finalize kernels are 64-rays-per-probe (8x8 groups); striding required before re-bump
    float ddgi_probe_spacing = 2.0f;
    float ddgi_irradiance_temporal_weight = 0.02f;
    float ddgi_depth_temporal_weight = 0.2f;
    float ddgi_ray_max_distance = 50.0f;

    // DDGI far-field coarse volume (T4.4): second layer so pixels outside the
    // near-field box still receive GI. Extent = counts × spacing (default
    // 16×8×16 @ 8 = 128×64×128 world units vs near 32×16×32 @ 2 = 64×32×64).
    bool ddgi_enable_far_volume = true;
    u32 ddgi_far_probe_count_x = 16;
    u32 ddgi_far_probe_count_y = 8;
    u32 ddgi_far_probe_count_z = 16;
    float ddgi_far_probe_spacing = 8.0f;
    float ddgi_far_ray_max_distance = 128.0f;

    // Surface Cache settings
    u32 surface_cache_atlas_size = 2048;
    u32 surface_cache_page_size = 32;               // 2048/32 = 64 pages per side = 4096 total pages
    u32 surface_cache_capture_budget = 256;          // Pages per frame
    u32 surface_cache_max_cards = 4096;
    float surface_cache_update_distance = 50.0f;     // Only update cards within this distance
    float surface_cache_importance_weight = 0.5f;    // Camera proximity vs recency tradeoff

    // GTAO settings
    float gtao_radius = 1.0f;
    float gtao_power = 1.5f;
    u32 gtao_direction_count = 4;
    u32 gtao_sample_count = 2;
    float gtao_temporal_weight = 0.85f;

    // SSDO settings
    float ssdo_radius = 0.5f;
    u32 ssdo_sample_count = 8;
    float ssdo_depth_bias = 0.01f;
    float ssdo_color_bleed_intensity = 0.3f;
    float ssdo_temporal_weight = 0.85f;

    // Shadow settings
    u32 shadow_cascade_count = 2;
    u32 shadow_map_resolution = 2048;
    float shadow_cascade_split_0 = 60.0f;
    float shadow_cascade_split_1 = 500.0f;
    ShadowQuality shadow_quality = ShadowQuality::PCF_16;

    // Screen Probes settings
    u32 screen_probes_spacing = 8;
    u32 screen_probes_rays = 64;

    // Performance budget (ms)
    float max_gi_time_ms = 6.0f;
};

// --- Static Probe Volume ---
struct StaticProbeParams {
    u32 grid_dim_x{16};
    u32 grid_dim_y{8};
    u32 grid_dim_z{16};
    float spacing{4.0f};
    math::v3 origin{0.0f};
};

struct ProbeConfidenceData {
    float ray_hit_ratio{0.0f};
    float temporal_stability{0.0f};
    float visibility_conf{0.0f};
    float convergence_age{0.0f};
};

} // namespace primal::graphics::lumen
