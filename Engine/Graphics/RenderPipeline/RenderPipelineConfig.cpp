#include "PipelineQualityConfig.h"
#include <cstddef>

namespace primal::graphics {

// ============================================================================
// RenderPassInfo
// ============================================================================

static const RenderPassInfo kPassInfos[] = {
    {"Shadow",             "Shadow map cascade rendering",               true},
    {"DeferredLighting",   "Deferred PBR lighting",                      true},
    {"SSAO",               "Screen-space ambient occlusion",             true},
    {"DDGI",               "Dynamic diffuse global illumination",        true},
    {"SurfaceCache",       "Surface cache card capture",                 false},
    {"SC-DDGI",            "Surface cache → DDGI integration",           false},
    {"SSGI",               "Screen-space global illumination",           true},
    {"SSR",                "Screen-space reflections",                   false},
    {"ScreenProbes",       "Screen probe GI",                            false},
    {"GIGather",           "DDGI → half-res screen texture",             true},
    {"VolumePass",         "Post-process volumetric fog",                false},
    {"VolumeRenderer",     "Forward proxy cube volume rendering",        false},
    {"FroxelFog",          "Frustum-aligned volumetric fog",             false},
    {"FluidRender",        "Splat-based fluid rendering",                false},
    {"FusionComposite",    "Scene + GI + volume composite",              true},
    {"FinalBlit",          "Final blit to backbuffer",                   true},
};

static_assert(sizeof(kPassInfos) / sizeof(kPassInfos[0]) == static_cast<u32>(RenderPassID::Count),
              "kPassInfos must match RenderPassID::Count");

const RenderPassInfo& GetRenderPassInfo(RenderPassID id) {
    return kPassInfos[static_cast<u32>(id)];
}

// ============================================================================
// PipelineQualityConfig — IsPassEnabled / SetPassEnabled
// ============================================================================

bool PipelineQualityConfig::IsPassEnabled(RenderPassID id) const {
    switch (id) {
        case RenderPassID::Shadow:             return enable_shadow;
        case RenderPassID::DeferredLighting:   return enable_deferred_lighting;
        case RenderPassID::SSAO:               return enable_ssao;
        case RenderPassID::DDGI:               return enable_ddgi;
        case RenderPassID::SurfaceCache:       return enable_surface_cache;
        case RenderPassID::SCDDGIIntegration:  return enable_surface_cache && enable_ddgi;
        case RenderPassID::SSGI:               return enable_ssgi;
        case RenderPassID::SSR:                return enable_ssr;
        case RenderPassID::ScreenProbes:       return enable_screen_probes;
        case RenderPassID::GIGather:           return enable_ddgi;
        case RenderPassID::VolumePass:         return enable_volume_pass;
        case RenderPassID::VolumeRenderer:     return enable_volume_renderer;
        case RenderPassID::FroxelFog:         return enable_froxel_fog;
        case RenderPassID::FluidRender:       return enable_fluid_render;
        case RenderPassID::FusionComposite:    return enable_ssgi || enable_ddgi || enable_ssr;
        case RenderPassID::FinalBlit:          return enable_final_blit;
        default: return false;
    }
}

void PipelineQualityConfig::SetPassEnabled(RenderPassID id, bool enabled) {
    switch (id) {
        case RenderPassID::Shadow:             enable_shadow = enabled; break;
        case RenderPassID::DeferredLighting:   enable_deferred_lighting = enabled; break;
        case RenderPassID::SSAO:               enable_ssao = enabled; break;
        case RenderPassID::DDGI:               enable_ddgi = enabled; break;
        case RenderPassID::SurfaceCache:       enable_surface_cache = enabled; break;
        case RenderPassID::SCDDGIIntegration:  break; // derived: enable SurfaceCache + DDGI instead
        case RenderPassID::SSGI:               enable_ssgi = enabled; break;
        case RenderPassID::SSR:                enable_ssr = enabled; break;
        case RenderPassID::ScreenProbes:       enable_screen_probes = enabled; break;
        case RenderPassID::GIGather:           break; // derived: enable DDGI instead
        case RenderPassID::VolumePass:         enable_volume_pass = enabled; break;
        case RenderPassID::VolumeRenderer:     enable_volume_renderer = enabled; break;
        case RenderPassID::FroxelFog:         enable_froxel_fog = enabled; break;
        case RenderPassID::FluidRender:       enable_fluid_render = enabled; break;
        case RenderPassID::FusionComposite:    break; // derived: enable SSGI or DDGI instead
        case RenderPassID::FinalBlit:          enable_final_blit = enabled; break;
        default: break;
    }
}

// ============================================================================
// PipelineQualityConfig — FromPreset
// ============================================================================

PipelineQualityConfig PipelineQualityConfig::FromPreset(lumen::LumenQualityPreset p) {
    PipelineQualityConfig cfg;
    cfg.preset = p;

    switch (p) {
    case lumen::LumenQualityPreset::Off:
        cfg.enable_ssao = false;
        cfg.enable_ssgi = false;
        cfg.enable_ddgi = false;
        cfg.enable_surface_cache = false;
        cfg.enable_screen_probes = false;
        cfg.shadow_quality = lumen::ShadowQuality::Hard;
        cfg.render_scale = 1.0f;
        break;

    case lumen::LumenQualityPreset::Low:
        cfg.enable_ssao = true;
        cfg.enable_ssgi = true;
        cfg.enable_ddgi = false;
        cfg.enable_surface_cache = false;
        cfg.enable_screen_probes = false;
        cfg.shadow_quality = lumen::ShadowQuality::Hard;
        cfg.render_scale = 1.0f;
        break;

    case lumen::LumenQualityPreset::Medium:
        cfg.enable_ssao = true;
        cfg.enable_ssgi = true;
        cfg.enable_ddgi = true;
        cfg.enable_surface_cache = false;
        cfg.enable_screen_probes = false;
        cfg.shadow_quality = lumen::ShadowQuality::PCF_16;
        cfg.render_scale = 1.0f;
        break;

    case lumen::LumenQualityPreset::High:
        cfg.enable_ssao = true;
        cfg.enable_ssgi = true;
        cfg.enable_ddgi = true;
        cfg.enable_surface_cache = true;
        cfg.enable_screen_probes = false;
        cfg.shadow_quality = lumen::ShadowQuality::PCSS;
        cfg.render_scale = 2.0f;
        break;

    case lumen::LumenQualityPreset::Ultra:
    case lumen::LumenQualityPreset::UltraRT:
        cfg.enable_ssao = true;
        cfg.enable_ssgi = true;
        cfg.enable_ddgi = true;
        cfg.enable_surface_cache = true;
        cfg.enable_screen_probes = true;
        cfg.shadow_quality = lumen::ShadowQuality::PCSS;
        cfg.render_scale = 2.0f;
        break;
    }

    return cfg;
}

// ============================================================================
// ParamDescriptor table
// ============================================================================

#define OFF(field) static_cast<u32>(offsetof(RenderPipelineSettings, field))
#define SZ(field)  static_cast<u32>(sizeof(((RenderPipelineSettings*)0)->field))

static const ParamDescriptor kParams[] = {
    // --- Pass toggles ---
    {"Enable Shadow",           "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_shadow),            SZ(quality.enable_shadow),            nullptr},
    {"Enable DeferredLighting", "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_deferred_lighting), SZ(quality.enable_deferred_lighting), nullptr},
    {"Enable SSAO",             "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_ssao),              SZ(quality.enable_ssao),              nullptr},
    {"Enable DDGI",             "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_ddgi),              SZ(quality.enable_ddgi),              nullptr},
    {"Enable SurfaceCache",     "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_surface_cache),     SZ(quality.enable_surface_cache),     nullptr},
    {"Enable SSGI",             "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_ssgi),              SZ(quality.enable_ssgi),              nullptr},
    {"Enable ScreenProbes",     "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_screen_probes),     SZ(quality.enable_screen_probes),     nullptr},
    {"Enable VolumePass",       "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_volume_pass),       SZ(quality.enable_volume_pass),       nullptr},
    {"Enable VolumeRenderer",   "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_volume_renderer),   SZ(quality.enable_volume_renderer),   nullptr},
    {"Enable FroxelFog",        "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_froxel_fog),        SZ(quality.enable_froxel_fog),        nullptr},
    {"Enable FluidRender",      "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_fluid_render),      SZ(quality.enable_fluid_render),      nullptr},
    {"Enable FinalBlit",        "Passes",  ParamType::Bool, {0,1,1},     OFF(quality.enable_final_blit),        SZ(quality.enable_final_blit),        nullptr},

    // --- Quality ---
    {"Render Scale",        "Quality",  ParamType::Float, {0.25f, 4.0f, 0.1f},  OFF(quality.render_scale),    SZ(quality.render_scale),    nullptr},
    {"Shadow Quality",      "Quality",  ParamType::Enum,  {0,2,1},               OFF(quality.shadow_quality),  SZ(quality.shadow_quality),  "Hard,PCF_16,PCSS"},

    // --- Lighting ---
    {"Light Direction",     "Lighting", ParamType::Vec3, {-1,1,0.01f},  OFF(lighting.light_direction),  SZ(lighting.light_direction),  nullptr},
    {"Light Color",         "Lighting", ParamType::Vec4, {0,50,0.1f},   OFF(lighting.light_color),      SZ(lighting.light_color),      nullptr},
    {"DDGI Light Color",    "Lighting", ParamType::Vec4, {0,50,0.1f},   OFF(lighting.ddgi_light_color), SZ(lighting.ddgi_light_color), nullptr},
    {"Cascade Split 0",     "Lighting", ParamType::Float, {10,5000,10},  OFF(lighting.cascade_splits),   sizeof(float),                  nullptr},
    {"Cascade Split 1",     "Lighting", ParamType::Float, {10,5000,10},  OFF(lighting.cascade_splits)+sizeof(float), sizeof(float), nullptr},

    // --- SSAO ---
    {"SSAO Radius",         "SSAO",  ParamType::Float, {0.1f, 10.0f, 0.1f}, OFF(lumen.gtao_radius),         SZ(lumen.gtao_radius),         nullptr},
    {"SSAO Power",          "SSAO",  ParamType::Float, {0.1f, 5.0f, 0.1f},  OFF(lumen.gtao_power),          SZ(lumen.gtao_power),          nullptr},
    {"SSAO Directions",     "SSAO",  ParamType::UInt,  {1, 16, 1},          OFF(lumen.gtao_direction_count), SZ(lumen.gtao_direction_count), nullptr},
    {"SSAO Samples",        "SSAO",  ParamType::UInt,  {1, 8, 1},           OFF(lumen.gtao_sample_count),    SZ(lumen.gtao_sample_count),    nullptr},

    // --- DDGI ---
    {"DDGI Probes X",       "DDGI",  ParamType::UInt,  {4, 64, 1},    OFF(lumen.ddgi_probe_count_x),         SZ(lumen.ddgi_probe_count_x),         nullptr},
    {"DDGI Probes Y",       "DDGI",  ParamType::UInt,  {4, 64, 1},    OFF(lumen.ddgi_probe_count_y),         SZ(lumen.ddgi_probe_count_y),         nullptr},
    {"DDGI Probes Z",       "DDGI",  ParamType::UInt,  {4, 64, 1},    OFF(lumen.ddgi_probe_count_z),         SZ(lumen.ddgi_probe_count_z),         nullptr},
    {"DDGI Rays/Probe",     "DDGI",  ParamType::UInt,  {8, 256, 8},   OFF(lumen.ddgi_rays_per_probe),        SZ(lumen.ddgi_rays_per_probe),        nullptr},
    {"DDGI Probe Spacing",  "DDGI",  ParamType::Float, {0.5f, 20.0f, 0.5f}, OFF(lumen.ddgi_probe_spacing),  SZ(lumen.ddgi_probe_spacing),         nullptr},
    {"DDGI Irradiance Weight", "DDGI", ParamType::Float, {0.001f, 1.0f, 0.001f}, OFF(lumen.ddgi_irradiance_temporal_weight), SZ(lumen.ddgi_irradiance_temporal_weight), nullptr},
    {"DDGI Depth Weight",   "DDGI",  ParamType::Float, {0.01f, 1.0f, 0.01f}, OFF(lumen.ddgi_depth_temporal_weight), SZ(lumen.ddgi_depth_temporal_weight), nullptr},
    {"DDGI Ray Max Dist",   "DDGI",  ParamType::Float, {5, 200, 5},   OFF(lumen.ddgi_ray_max_distance),      SZ(lumen.ddgi_ray_max_distance),      nullptr},

    // --- SSGI ---
    {"SSGI Rays",           "SSGI",  ParamType::UInt,  {1, 16, 1},    OFF(lumen.ssgi_ray_count),             SZ(lumen.ssgi_ray_count),             nullptr},
    {"SSGI Radius",         "SSGI",  ParamType::Float, {0.5f, 10.0f, 0.1f}, OFF(lumen.ssgi_radius),        SZ(lumen.ssgi_radius),                 nullptr},
    {"SSGI Temporal Weight","SSGI",  ParamType::Float, {0.5f, 1.0f, 0.01f}, OFF(lumen.ssgi_temporal_weight), SZ(lumen.ssgi_temporal_weight),    nullptr},

    // --- Volume ---
    {"Volume Step Size",       "Volume", ParamType::Float, {0.01f, 5.0f, 0.01f}, OFF(volume.step_size),        SZ(volume.step_size),        nullptr},
    {"Volume Max Distance",    "Volume", ParamType::Float, {10, 500, 10},         OFF(volume.max_distance),     SZ(volume.max_distance),     nullptr},
    {"Volume Extinction",      "Volume", ParamType::Float, {0.001f, 5.0f, 0.01f}, OFF(volume.extinction_scale), SZ(volume.extinction_scale), nullptr},
    {"Volume Max Steps",       "Volume", ParamType::UInt,  {8, 256, 8},           OFF(volume.max_steps),        SZ(volume.max_steps),        nullptr},

    // --- Bloom (schema-only; pass-side wiring pending) ---
    {"Bloom Intensity",        "Bloom",  ParamType::Float, {0.0f, 8.0f, 0.05f},   OFF(bloom.intensity),         SZ(bloom.intensity),         nullptr},
    {"Bloom Threshold",        "Bloom",  ParamType::Float, {0.0f, 4.0f, 0.05f},   OFF(bloom.threshold),         SZ(bloom.threshold),         nullptr},
    {"Bloom Radius",           "Bloom",  ParamType::Float, {0.05f, 1.0f, 0.01f},  OFF(bloom.radius),            SZ(bloom.radius),            nullptr},

    // --- TAA (schema-only; pass-side wiring pending) ---
    {"TAA Sharpness",          "TAA",    ParamType::Float, {0.0f, 1.0f, 0.01f},   OFF(taa.sharpness),           SZ(taa.sharpness),           nullptr},
    {"TAA Feedback",           "TAA",    ParamType::Float, {0.0f, 1.0f, 0.01f},   OFF(taa.feedback),            SZ(taa.feedback),            nullptr},
};

#undef OFF
#undef SZ

const ParamDescriptor* GetParamDescriptors(u32& out_count) {
    out_count = sizeof(kParams) / sizeof(kParams[0]);
    return kParams;
}

} // namespace primal::graphics
