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
// PipelineQualityConfig — Pass 开关 + 质量设置
// ============================================================================
struct PipelineQualityConfig {
    lumen::LumenQualityPreset preset = lumen::LumenQualityPreset::Medium;

    // Pass toggles
    bool enable_ssao              = true;
    bool enable_ssgi              = true;
    bool enable_ddgi              = true;
    bool enable_surface_cache     = false;
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
