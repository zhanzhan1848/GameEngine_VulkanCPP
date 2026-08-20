#pragma once

#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/Volume/FroxelTypes.h"
#include "Graphics/Fluid/FluidTypes.h"
#include "Utilities/MathTypes.h"

namespace primal::graphics {

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
    math::v4  ddgi_light_color = {20.0f, 20.0f, 20.0f, 1.0f};
    math::v4  cascade_splits   = {600.0f, 2000.0f, 0.0f, 0.0f};
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
// Bloom 参数 — schema-only (settings + descriptors); not yet consumed by
// BloomPass.cpp. Adding the schema lets Editor UI generate controls; pass-side
// wiring is a separate task (TODO: thread BloomConfig through AddBloomPass).
// ============================================================================
struct BloomConfig {
    float intensity = 1.0f;   // multiplier on the blurred bloom contribution
    float threshold = 1.0f;   // luminance above which pixels feed the bright pass
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
    bool enable_ssr               = false;
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
