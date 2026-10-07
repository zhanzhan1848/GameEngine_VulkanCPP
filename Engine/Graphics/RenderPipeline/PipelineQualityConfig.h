#pragma once

#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/Volume/FroxelTypes.h"
#include "Graphics/Fluid/FluidTypes.h"
#include "Utilities/MathTypes.h"

namespace primal::graphics {

// ============================================================================
// [R3/R4/R5 2026-09-20] 反射档位（PRIMAL_REFL，SC 正确性 × Reflections 整合
// 计划的统一三档验收位）：0 = off；1 = v1（SSR 默认开 + DDGI glossy relay
// 兜底）；2 = v2-v3（+ WRC 探针 specular 采样 / SC 卡片 specular trace，
// ReflSpecularTrace pass）。PRIMAL_SSR=0 为 v1 内的 SSR 单独回退位。
// ============================================================================
inline u32 ReflectionLevel() {
    static const u32 v = [] {
        const char* e = std::getenv("PRIMAL_REFL");
        if (!e) return 1u;
        if (e[0] == '0') return 0u;
        if (e[0] == '2') return 2u;
        return 1u;
    }();
    return v;
}
/// [R4] WRC 探针 specular 采样原型开关（PRIMAL_REFL_PROBE=1，与档位 2 叠加）。
inline bool ReflProbeEnabled() {
    static const bool v = [] {
        const char* e = std::getenv("PRIMAL_REFL_PROBE");
        return e && e[0] == '1';
    }();
    return v;
}

// ============================================================================
// Pass ID — 用于 SetPassEnabled / IsPassActive / UI 遍历
// ============================================================================
enum class RenderPassID : u32 {
    Shadow,
    DeferredLighting,
    SSAO,
    DDGI,
    SurfaceCache,
    SCDDGIIntegration,
    SSGI,
    SSR,
    ScreenProbes,
    GIGather,
    VolumePass,
    VolumeRenderer,
    FroxelFog,
    FluidRender,
    FusionComposite,
    FinalBlit,
    Count
};

struct RenderPassInfo {
    const char* name;
    const char* description;
    bool default_enabled;
};

const RenderPassInfo& GetRenderPassInfo(RenderPassID id);

// ============================================================================
// 场景光照参数 — 当前硬编码在 StandardRenderPipeline.cpp 中
// ============================================================================
struct SceneLightingParams {
    math::v3  light_direction  = {0.707f, -1.0f, 0.408f};
    math::v4  light_color      = {5.0f, 5.0f, 5.0f, 1.0f};
    // NOTE: the Vulkan Lumen chain pairs ddgi_light_color with the screen-side
    // gather ×π correction (DDGIGIGather.comp PROBE_SH_RAD_TO_IRRAD); scenes
    // feeding that path divide this by π explicitly for now
    // (TestVulkanSponzaRenderGraph.cpp). Default folds in once M3 归一轮
    // aligns Metal/Dawn gathers with the Vulkan semantics.
    math::v4  ddgi_light_color = {20.0f, 20.0f, 20.0f, 1.0f};
    math::v4  cascade_splits   = {600.0f, 2000.0f, 0.0f, 0.0f};
    // 2026-09-17 sun-audit §5.3: PCSS 的光源角直径（度）。半影只由它和
    // blocker–receiver 世界距离决定，与太阳方向（方位/高度）是两个量。
    // 0.5357° = 物理日光起始值；置 0 回退旧的固定 lightSizeUV=0.012 行为。
    float sun_angular_diameter_deg = 0.5357f;
};

// ============================================================================
// 管线高级参数 — 当前硬编码
// ============================================================================
struct PipelineAdvancedParams {
    u32   shadow_map_resolution = 2048;
    u32   shadow_num_instances  = 1000;
    u32   shadow_max_clusters   = 10000;
    float delta_time_override   = 0.016f;
};

// ============================================================================
// Bloom 参数 — P3 接线（sponza-gap-audit §4.9）: bright-pass 的
// threshold/knee/energy 已贯穿到 Bloom.frag 的 UBO（StandardRenderPipeline
// 的 MakeBloomParams 从 PRIMAL_BLOOM_THRESHOLD/KNEE/ENERGY 读取，默认与
// 下方标定值一致）；intensity 的合成端对应 ToneMappingPass 的
// PRIMAL_BLOOM_INTENSITY。runtime 目前以 env 为主入口，schema 值保持一致
// 以便编辑器 UI 后续直接接管。
// ============================================================================
struct BloomConfig {
    float intensity = 1.0f;   // multiplier on the blurred bloom contribution
    float threshold = 4.0f;   // HDR luminance center of the soft-knee transition
                              // (was 1.0 — dead default; 4.0 matches the shipped
                              //  bright-pass calibration; sky sits at ~4.2-6.0)
    float knee      = 1.0f;   // transition half-width
    float radius    = 0.6f;   // gaussian blur radius (texture-space fraction)
};

// ============================================================================
// TAA 参数 — schema-only (settings + descriptors); not yet consumed by
// TAAPass.cpp (feedback currently hardcoded in TAAUniforms).
// ============================================================================
struct TAAConfig {
    float sharpness = 0.8f;   // neighborhood-clamping sharpness (0..1)
    float feedback  = 0.9f;   // history blend factor (0..1, higher = smoother)
};

// ============================================================================
// SSR 参数 — Screen-Space Reflections 调参。由 StandardRenderPipeline 传入
// AddSSRPass，并在 FusionComposite 合成时按粗糙度衰减。
// ============================================================================
struct SSRConfig {
    float reflection_strength = 0.7f;   ///< 反射整体强度倍率
    float max_roughness       = 0.85f;  ///< 粗糙度 > 此值的像素不反射（早退）
    float fresnel_power       = 3.0f;   ///< Fresnel 指数（ grazing 角度反射增强 ）
    float max_trace_distance  = 30.0f;  ///< Hi-Z 光线步进最大距离（view-space units）
    float thickness           = 0.5f;   ///< 层厚度（ ray-hit 碰撞判定，小值防穿透重影 ）
    float temporal_feedback   = 0.88f;  ///< 时间累积权重（越高越平滑，0..1）
};

// ============================================================================
// PipelineQualityConfig — Pass 开关 + 质量设置
// ============================================================================
struct PipelineQualityConfig {
    lumen::LumenQualityPreset preset = lumen::LumenQualityPreset::Medium;

    // Pass toggles
    bool enable_ssao              = true;
    bool enable_ssgi              = true;
    // [R3 2026-09-20] Reflections v1：SSR 默认开（屏幕内反射；屏幕外由
    // ddgiGlossy relay 兜底，v2/v3 再接 WRC 探针/SC 卡片 specular trace）。
    // PRIMAL_REFL=0 三档总闸回退；PRIMAL_SSR=0 单独回退。
    bool enable_ssr               = [] {
        if (ReflectionLevel() == 0u) return false;
        const char* e = std::getenv("PRIMAL_SSR");
        return !(e && e[0] == '0');
    }();
    bool enable_ddgi              = true;
    bool enable_surface_cache     = true;   // Vulkan AtlasInit path now produces valid atlas data
    bool enable_screen_probes     = false;
    bool enable_shadow            = true;
    bool enable_deferred_lighting = true;
    bool enable_final_blit        = true;
    bool enable_volume_pass       = false;
    bool enable_volume_renderer   = false;
    bool enable_froxel_fog        = false;
    bool enable_fluid_render      = false;

    // Quality settings
    lumen::ShadowQuality shadow_quality = lumen::ShadowQuality::PCF_16;
    float render_scale = 1.0f;

    // Enum → bool mapping
    bool IsPassEnabled(RenderPassID id) const;
    void SetPassEnabled(RenderPassID id, bool enabled);

    static PipelineQualityConfig FromPreset(lumen::LumenQualityPreset p);

    // T4.5/C4: axis-schema mapping — one source of truth for preset → Lumen
    // axis parameters (probe grid / rays / far volume / SC budgets / probes).
    // FromPreset and editor UI both consume this so pass toggles and GI axis
    // values can never drift apart.
    static lumen::LumenConfig LumenConfigFromPreset(lumen::LumenQualityPreset p);
};

// ============================================================================
// 统一设置 struct — 包含所有可调参数
// ============================================================================
struct RenderPipelineSettings {
    PipelineQualityConfig  quality;
    lumen::LumenConfig     lumen;
    SceneLightingParams    lighting;
    PipelineAdvancedParams advanced;
    BloomConfig            bloom;
    TAAConfig              taa;
    SSRConfig              ssr;
    volume::VolumeRuntimeParams volume;
    volume::FroxelGridConfig froxel;
    fluid::FluidConfig fluid;

    // T4.5/C4: one call — pass toggles and Lumen axis values from the same
    // preset (both route through FromPreset / LumenConfigFromPreset).
    static RenderPipelineSettings FromPreset(lumen::LumenQualityPreset p) {
        RenderPipelineSettings s{};
        s.quality = PipelineQualityConfig::FromPreset(p);
        s.lumen = PipelineQualityConfig::LumenConfigFromPreset(p);
        return s;
    }
};

// ============================================================================
// 参数描述符 — 供 UI 自动生成控件
// ============================================================================
enum class ParamType : u32 { Bool, Float, Int, UInt, Enum, Vec3, Vec4 };

struct ParamRange {
    float min_val = 0.0f;
    float max_val = 1.0f;
    float step    = 0.01f;
};

struct ParamDescriptor {
    const char* name;
    const char* group;
    ParamType   type;
    ParamRange  range;
    u32         offset;
    u32         size;
    const char* enum_names;
};

const ParamDescriptor* GetParamDescriptors(u32& out_count);

} // namespace primal::graphics
