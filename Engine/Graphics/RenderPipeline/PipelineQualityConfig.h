#pragma once

#include "Graphics/Lumen/LumenTypes.h"

namespace primal::graphics {

struct PipelineQualityConfig {
    lumen::LumenQualityPreset preset = lumen::LumenQualityPreset::Medium;

    bool enable_ssao = true;
    bool enable_ssgi = true;
    bool enable_ddgi = true;
    bool enable_surface_cache = false;
    bool enable_screen_probes = false;
    lumen::ShadowQuality shadow_quality = lumen::ShadowQuality::PCF_16;

    static PipelineQualityConfig FromPreset(lumen::LumenQualityPreset p) {
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
            break;

        case lumen::LumenQualityPreset::Low:
            cfg.enable_ssao = true;
            cfg.enable_ssgi = true;
            cfg.enable_ddgi = false;
            cfg.enable_surface_cache = false;
            cfg.enable_screen_probes = false;
            cfg.shadow_quality = lumen::ShadowQuality::Hard;
            break;

        case lumen::LumenQualityPreset::Medium:
            cfg.enable_ssao = true;
            cfg.enable_ssgi = true;
            cfg.enable_ddgi = true;
            cfg.enable_surface_cache = false;
            cfg.enable_screen_probes = false;
            cfg.shadow_quality = lumen::ShadowQuality::PCF_16;
            break;

        case lumen::LumenQualityPreset::High:
            cfg.enable_ssao = true;
            cfg.enable_ssgi = true;
            cfg.enable_ddgi = true;
            cfg.enable_surface_cache = true;
            cfg.enable_screen_probes = false;
            cfg.shadow_quality = lumen::ShadowQuality::PCSS;
            break;

        case lumen::LumenQualityPreset::Ultra:
            cfg.enable_ssao = true;
            cfg.enable_ssgi = true;
            cfg.enable_ddgi = true;
            cfg.enable_surface_cache = true;
            cfg.enable_screen_probes = true;
            cfg.shadow_quality = lumen::ShadowQuality::PCSS;
            break;

        case lumen::LumenQualityPreset::UltraRT:
            cfg.enable_ssao = true;
            cfg.enable_ssgi = true;
            cfg.enable_ddgi = true;
            cfg.enable_surface_cache = true;
            cfg.enable_screen_probes = true;
            cfg.shadow_quality = lumen::ShadowQuality::PCSS;
            break;
        }

        return cfg;
    }
};

} // namespace primal::graphics
