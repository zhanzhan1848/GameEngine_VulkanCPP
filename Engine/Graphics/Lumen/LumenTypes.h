#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "GICacheView.h"   // X3/A6: GITemporalStrategy（X2 已立的轴5枚举）
#include <vector>

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
// --- Multi-volume runtime (T5.5, cap 4) ---
// Entry 0 = primary near volume; entry 1 = far-field coarse shell (T4.4).
// Entries 2..3 are user placements (editor/M6). When `overrides` is empty the
// pipeline derives entries 0/1 from the ddgi_* fields above (backward compat).
struct LumenVolumeConfig {
    u32    probe_count_x = 32;
    u32    probe_count_y = 16;
    u32    probe_count_z = 32;
    float  probe_spacing = 2.0f;
    u32    rays_per_probe = 64;
    float  ray_max_distance = 50.0f;
    u32    update_budget_divisor = 1;   // probe budget = near budget / divisor
    math::v3 origin_override{0.0f, 0.0f, 0.0f};  // (0,0,0) = world-centered
    bool   use_origin_override = false;
};

// ============================================================================
// X3/A6: 五轴显式配置 —— 每轴独立可选，M6 UI 按轴生成控件。
// 五轴定义见 Docs/2026-08-24-gi-optimization-plan.md §1.3/§2：
//   空间缓存 × 方向参数化 × 光学量/积分时机 × 更新传播拓扑 × 时域策略
// 轴2/轴3 是 kernel 分支轴（config 字段落位，分支点随 M6/T6.3 CB 化）；
// 轴1/轴4/轴5 由 StandardRenderPipeline 接线到现有运行时机制（ApplyGIaxes）。
// 与 GIDebugParams 的关系：axes 是 init 时配置面（LumenConfig 内嵌，
// SetLumenConfig 生效）；GIDebug 是运行时热调面（进程级单例）。
// ============================================================================
struct GIFiveAxisConfig {
    // 轴1 空间缓存组合（与 PipelineQualityConfig 的 quality.enable_* 联动，
    // 此字段为轴视图单一真源；LumenConfigFromPreset 按档位填充两表）
    bool use_surface_cache = true;
    bool use_ddgi_volume = true;
    bool use_screen_probes = false;   // High+ 预设翻 true

    // 轴2 方向参数化（kernel 分支轴：SH2=屏幕探针降级档，SH3=DDGI/Gather 现状）
    enum class DirectionalParameterization { SH2, SH3 };
    DirectionalParameterization directional = DirectionalParameterization::SH3;

    // 轴3 光学量/积分时机（kernel 分支轴）：Analytic=Karis 解析 EnvBRDF
    // （T4.3 现状）；PreintegratedLUT=DFG LUT 纹理（IBL 接线时切换）
    enum class DFGPath { Analytic, PreintegratedLUT };
    DFGPath dfg_path = DFGPath::Analytic;

    // 轴4 更新传播拓扑（SC→DDGI 喂食边 / SPGI miss→DDGI fallback 边）
    bool propagate_sc_to_ddgi = true;
    bool fallback_spgi_to_ddgi = true;

    // 轴5 时域策略（X2 已立枚举；EMA=现状默认）
    GITemporalStrategy temporal = GITemporalStrategy::EMA;
};

struct LumenConfig {
    LumenQualityPreset quality = LumenQualityPreset::Medium;

    // X3/A6: 五轴显式配置（默认值组合 = 现状 Medium 行为）
    GIFiveAxisConfig axes{};

    // SSGI settings
    u32 ssgi_ray_count = 4;
    float ssgi_radius = 2.0f;
    float ssgi_temporal_weight = 0.95f;

    // DDGI settings
    // DDGI probe grid — known-working config for Sponza.
    u32 ddgi_probe_count_x = 32;
    u32 ddgi_probe_count_y = 16;
    u32 ddgi_probe_count_z = 32;
    u32 ddgi_rays_per_probe = 128;   // T8.13 P2: 64→128 — lane-strided kernels (r += 64) landed in TraceSDF/Finalize/TraceRays. P0 实测 DDGI 全子过程 1.6ms，翻倍预算充裕
    // 3.0 covers ±46×±22×±46 around the origin — Sponza's side arcades extend
    // past the old ±31 box, and out-of-box pixels fall to the far volume whose
    // probes have no SC feed (near-black in enclosed areas), which read as
    // dead columns/cloth. Same probe count, half the density.
    float ddgi_probe_spacing = 3.0f;
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

    // T5.5: explicit volume list (cap 4). Empty → derive 0/1 from the fields
    // above. Non-empty → overrides win; entry 0 must be the near volume.
    std::vector<LumenVolumeConfig> volume_overrides;

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
    bool screen_probes_jitter = true;   // F5 (M7): per-frame probe/ray jitter
    float screen_probes_far_cache_start = 30.0f;  // F1 (M7): SDF far-field early-out → DDGI cache query (0 disables)
    bool screen_probes_importance = true;  // F2 (M7): importance resampling of ray directions (history-SH guided)
    float screen_probes_adaptive_pos_error = 0.35f;  // F4 (M7): fine-tier interpolation-failure threshold (0 → legacy variance test)

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
