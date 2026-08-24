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
// ProbeCardContrib = 20B tight, ProbeContribRange = 8B tight.
// ============================================================================

struct SCProbeCardContrib {
    u32  card_index;
    float sh_weights[4];   // pre-computed L0+L1 SH projection weights
};

struct SCProbeContribRange {
    u32 start;
    u32 count;
};

constexpr u32 SC_MAX_CONTRIBS_PER_PROBE = 6;

struct SCDDGIInputs {
    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::SurfaceCachePass* surface_cache_pass = nullptr;

    u32 current_buffer_index = 0;
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
};

} // namespace primal::graphics
