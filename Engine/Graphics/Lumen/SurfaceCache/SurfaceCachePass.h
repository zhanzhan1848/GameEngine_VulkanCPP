#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "SurfaceCacheTypes.h"
#include "CardGenerator.h"
#include "../LumenTypes.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

// ============================================================================
// SurfaceCachePass — orchestrates all surface cache sub-passes
// ============================================================================

/**
 * @brief Self-contained surface cache pass: CardCapture -> DepthDilate -> LightCull -> LightEval -> IndirectTrace -> IndirectResolve.
 *
 * Lifecycle:
 *   1. Initialize(device, config)       -- once
 *   2. AddPass(graph, ..., frame_data)  -- every frame
 *   3. Shutdown()                        -- once
 *
 * The pass owns all persistent GPU resources:
 *   - Atlas textures (albedo, normal, depth, emissive, lighting)
 *   - Triple-buffered constant buffers and lighting atlas
 *   - Compute pipelines (stubs until later tasks)
 *   - Runtime buffers for light assignment, light info, ray hits
 */
class SurfaceCachePass {
public:
    SurfaceCachePass() = default;
    ~SurfaceCachePass();

    bool Initialize(rhi::RHIDeviceBase* device, const LumenConfig& config);
    void Shutdown();

    SurfaceCacheOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle prev_frame_color,
        rhi::ResourceHandle light_data_buffer,
        const SurfaceCacheFrameData& frame_data,
        u32 current_frame_index);

    rhi::ResourceHandle GetLightingAtlas(u32 frame_index) const {
        return lighting_atlas_[frame_index % 3];
    }
    rhi::ResourceHandle GetAlbedoAtlas() const { return albedo_atlas_; }
    rhi::ResourceHandle GetNormalAtlas() const { return normal_atlas_; }
    rhi::ResourceHandle GetDepthAtlas() const { return depth_atlas_; }
    rhi::ResourceHandle GetEmissiveAtlas() const { return emissive_atlas_; }
    rhi::ResourceHandle GetLightAssignmentBuffer() const { return light_assignment_buffer_; }
    rhi::ResourceHandle GetLightInfoBuffer() const { return light_info_buffer_; }
    u32 GetAtlasSize() const { return atlas_size_; }
    u32 GetPageSize() const { return page_size_; }
    rhi::ResourceHandle GetCardDataBuffer() const {
        return card_generator_.GetCardDataBuffer();
    }
    rhi::ResourceHandle GetCardLookupBuffer() const {
        return card_generator_.GetCardLookupBuffer();
    }

    bool IsInitialized() const { return initialized_; }

    CardGenerator& GetCardGenerator() { return card_generator_; }
    const CardGenerator& GetCardGenerator() const { return card_generator_; }

private:
    void CreateAtlasTextures();
    void CreateConstantBuffers();
    void CreateDescriptorSetLayouts();
    void CreatePipelines();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    LumenConfig       config_{};

    CardGenerator     card_generator_;

    // Atlas textures (persistent)
    rhi::ResourceHandle albedo_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle normal_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle depth_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle emissive_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle lighting_atlas_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle prev_lighting_atlas_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle global_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle dilate_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_cull_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_eval_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle capture_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle indirect_trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle indirect_resolve_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Pipelines
    rhi::PipelineHandle capture_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle dilate_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_cull_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_eval_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle capture_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle dilate_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_cull_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_eval_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_resolve_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor sets (triple-buffered per pass)
    rhi::DescriptorSetHandle capture_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle dilate_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_cull_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_eval_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle indirect_trace_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle indirect_resolve_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Runtime buffers
    rhi::ResourceHandle light_assignment_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle light_info_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle ray_hits_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle card_dispatch_buffer_{ rhi::handles::INVALID_RESOURCE };

    u32 atlas_size_ = 0;
    u32 page_size_ = 0;
};

} // namespace primal::graphics::lumen
