#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class SurfaceCachePass; }

// ============================================================================
// SC → DDGI card→probe projection data (T2.1).
// Layouts must match DDGIProbeIrradianceFromCards.metal / .comp byte-for-byte:
// ProbeCardContrib = 40B tight (card_index + 9 SH weights),
// ProbeContribRange = 8B tight.
// ============================================================================

struct SCProbeCardContrib {
    u32  card_index;
    float sh_weights[9];   // pre-computed L0-L2 (9-band) SH projection weights
};

struct SCProbeContribRange {
    u32 start;
    u32 count;
};

// L2 projection from discrete card samples needs more than 6 directions to
// stay well-conditioned; 12 top-weight cards per probe keeps it stable.
constexpr u32 SC_MAX_CONTRIBS_PER_PROBE = 12;

struct SCDDGIInputs {
    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::SurfaceCachePass* surface_cache_pass = nullptr;

    // Slot base for triple-buffered GPU resources (lighting atlas, DDGI
    // irradiance/confidence/update-list). Must be the SAME counter passed to
    // LumenDDGIPass::AddPass / SurfaceCachePass::AddPass (the render frame
    // count) — the swapchain buffer index can phase-shift against it and
    // would bind slots that were never written this frame.
    u32 frame_index = 0;
    u32 current_buffer_index = 0;  // descriptor-set rotation only
    rendergraph::RGResourceHandle sc_lighting_rg;        // SC lighting atlas dependency
    rendergraph::RGResourceHandle ddgi_irradiance;       // ordering guard: DDGI writes first
    rendergraph::RGResourceHandle ddgi_irradiance_hist;  // (this pass read+modifies both)
};

class SCDDGIIntegrationModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle card_radiance_shader,
                    rhi::ShaderHandle probe_irradiance_shader,
                    u32 render_width, u32 render_height);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const SCDDGIInputs& inputs);

private:
    // T2.1: lazily create the card→probe projection buffers and build the
    // CPU-side contribution lists (once per scene — cards and the DDGI grid
    // are static between pass rebuilds).
    bool EnsureResources(lumen::LumenDDGIPass* ddgi, lumen::SurfaceCachePass* sc);
    bool BuildCardProbeAssignment(lumen::LumenDDGIPass* ddgi, lumen::SurfaceCachePass* sc);

    rhi::RHIDeviceBase* device_ = nullptr;

    rhi::PipelineHandle card_radiance_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle card_radiance_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle card_radiance_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle card_radiance_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    rhi::PipelineHandle probe_irradiance_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle probe_irradiance_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle probe_irradiance_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle probe_irradiance_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // GPU resources (created on first valid frame — counts come from the passes)
    rhi::ResourceHandle card_radiance_buf_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle probe_contrib_range_buf_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle flat_contrib_buf_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle sc_params_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle ddgi_vol_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    u32  total_probes_ = 0;
    u32  max_cards_ = 0;
    bool assignment_built_ = false;
    // T8.13 fix (Metal two-state GI flip): the SC card set is rebuilt at
    // runtime (placeholder cards -> streamed scene cards), which shuffles
    // card indices. The per-probe contribution assignment baked card indices
    // ONCE, so after a rebuild the injection aggregated WRONG cards'
    // radiance — intermittent probe corruption. Track the count and rebuild.
    u32 assignment_card_count_ = 0;

    // Debug (SC_DUMP_CARDS=1): staging copy of card_radiance_buf_ + the frame
    // its copy was enqueued; the print happens >=3 frames later so the copy
    // is guaranteed retired (static-scene diagnostics only).
    rhi::ResourceHandle card_radiance_staging_{rhi::handles::INVALID_RESOURCE};
    u32  dump_pending_frame_ = 0xFFFFFFFFu;
    u32  dump_card_count_ = 0;

    // Debug (same env): staging copy of the DDGI irradiance output (probe L0
    // time series — distinguishes "cards drained" from "probes drained").
    rhi::ResourceHandle ddgi_irr_staging_{rhi::handles::INVALID_RESOURCE};
    u32  dump_probe_count_ = 0;
};

} // namespace primal::graphics
