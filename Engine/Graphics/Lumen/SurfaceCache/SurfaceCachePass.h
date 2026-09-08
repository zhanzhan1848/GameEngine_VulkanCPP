#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "SurfaceCacheTypes.h"
#include "CardGenerator.h"
#include "../LumenTypes.h"
#include "../GICacheView.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics {
class RenderSceneSnapshot;
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

    // TEMP diagnosis (T8.13 Metal sync): direct CPU read of the SC shadow
    // visibility buffer (Dynamic/shared memory). Returns -1 when unavailable.
    float DebugShadowVisibilityAt(u32 index) const;
    // T3.3 诊断：可见性缓冲统计（均值/零值占比）——直射项归零排查。
    void DebugShadowVisibilityStats() const;
    bool DebugShadowPipelineReady() const { return shadow_pipeline_ != rhi::handles::INVALID_PIPELINE; }

    // T8.13: offline merged SDF (384^3) for the per-texel shadow march —
    // the runtime cascades (1m/2m/4m) misplace SC shadow hits the same way
    // they do for the DDGI trace.
    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution) {
        sc_offline_sdf_texture_ = texture;
        sc_offline_sdf_origin_ = origin;
        sc_offline_sdf_extent_ = extent;
        sc_offline_sdf_resolution_ = resolution;
    }

    bool Initialize(rhi::RHIDeviceBase* device, const LumenConfig& config);
    void Shutdown();

    SurfaceCacheOutput AddPass(
        rendergraph::RenderGraph& graph,
        rendergraph::RGResourceHandle prev_frame_color,
        rhi::ResourceHandle light_data_buffer,
        const SurfaceCacheFrameData& frame_data,
        u32 current_frame_index);

    // T8.12: card-view capture — renders a rotating batch of cards from their
    // own orthographic views straight into the atlas pages (albedo/normal/
    // axis-depth), replacing the camera-bound screen-space re-projection.
    // Enabled via env SC_CARDVIEW=1; when on, AddPass skips the screen-space
    // capture_gather dispatch (the two paths are mutually exclusive).
    bool IsCardViewEnabled() const { return card_view_enabled_; }
    void AddCardCapturePasses(rendergraph::RenderGraph& graph,
                              const RenderSceneSnapshot& scene_snapshot,
                              u32 current_frame_index);

    rhi::ResourceHandle GetLightingAtlas(u32 frame_index) const {
        return lighting_atlas_[frame_index % 3];
    }
    rhi::ResourceHandle GetAlbedoAtlas() const { return albedo_atlas_; }
    rhi::ResourceHandle GetNormalAtlas() const { return normal_atlas_; }
    rhi::ResourceHandle GetDepthAtlas() const { return depth_atlas_; }
    rhi::ResourceHandle GetEmissiveAtlas() const { return emissive_atlas_; }
    rhi::ResourceHandle GetLightInfoBuffer() const { return light_info_buffer_; }
    u32 GetAtlasSize() const { return atlas_size_; }
    u32 GetPageSize() const { return page_size_; }

    // T4.1: runtime LightEval page budget (GIAdaptiveGovernor). 0 keeps the
    // legacy full-update path; takes effect at the next AddPass.
    void SetPageBudget(u32 pages) { config_.surface_cache_capture_budget = pages; }
    rhi::ResourceHandle GetCardDataBuffer() const {
        return card_generator_.GetCardDataBuffer();
    }
    rhi::ResourceHandle GetCardLookupBuffer() const {
        return card_generator_.GetCardLookupBuffer();
    }
    rhi::ResourceHandle GetGridOffsetsBuffer() const {
        return card_generator_.GetGridOffsetsBuffer();
    }
    rhi::ResourceHandle GetGridCardsBuffer() const {
        return card_generator_.GetGridCardsBuffer();
    }

    // X2/A3: 统一 Cache View —— 消费者（DDGI finalize、SPGI）从这里取 SC 数据
    // 快照，不再逐个摸 pass 内部 buffer getter。纯数据搬运，逐字段等价于上方
    // 现有 getter 的组合；lighting_rg 由管线侧按需回填。
    SurfaceCacheView GetCacheView(u32 atlas_idx) const {
        SurfaceCacheView view;
        view.lighting_atlas = GetLightingAtlas(atlas_idx);
        view.card_data = card_generator_.GetCardDataBuffer();
        view.card_lookup = card_generator_.GetCardLookupBuffer();
        view.grid_offsets = card_generator_.GetGridOffsetsBuffer();
        view.grid_cards = card_generator_.GetGridCardsBuffer();
        view.depth_atlas = depth_atlas_;               // T3.4 (D3)
        view.atlas_size = atlas_size_;                 // == LumenConfig::surface_cache_atlas_size
        view.card_count = card_generator_.GetCardCount();
        view.max_card_count = card_generator_.GetMaxCardCount();
        view.lookup_count = card_generator_.GetLookupCount();
        return view;
    }

    // X2/A3: 时域策略枚举存储（SC 自身无 probe 时域链；X3 归并入五轴配置）。
    void SetTemporalStrategy(GITemporalStrategy strategy);
    GITemporalStrategy GetTemporalStrategy() const { return temporal_strategy_; }

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

    // T8.12 card-view capture state
    bool card_view_enabled_{ false };
    u32  card_view_cursor_{ 0 };          // face cursor over card indices
    bool card_view_seeded_{ false };      // all faces captured at least once
    u32  card_view_faces_total_{ 0 };
    rhi::ResourceHandle card_view_depth_{ rhi::handles::INVALID_RESOURCE };  // scratch D32 (atlas-sized)

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
    rhi::DescriptorSetLayoutHandle atlas_init_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle capture_gather_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle card_fill_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle shadow_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };   // T3.3

    // Pipelines
    rhi::PipelineHandle capture_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle dilate_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_cull_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_eval_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle indirect_resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle atlas_init_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle capture_gather_pipeline_{ rhi::handles::INVALID_PIPELINE };
    // A7: write/clear phases of the three-dispatch capture (same comp file)
    rhi::PipelineHandle capture_write_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle capture_clear_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle card_fill_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle shadow_pipeline_{ rhi::handles::INVALID_PIPELINE };   // T3.3

    // Pipeline layouts
    rhi::PipelineLayoutHandle capture_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle dilate_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_cull_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_eval_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle indirect_resolve_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle atlas_init_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle capture_gather_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle card_fill_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle shadow_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };   // T3.3

    // Descriptor sets (triple-buffered per pass)
    rhi::DescriptorSetHandle atlas_init_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle capture_gather_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle card_fill_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle shadow_set_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };   // T3.3
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
    rhi::ResourceHandle light_info_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle ray_hits_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle card_dispatch_buffer_{ rhi::handles::INVALID_RESOURCE };
    // A7: nearest-depth scratch (u32 floatBits per atlas texel) for the
    // three-dispatch GBuffer-gather capture (clear → atomicMin → write).
    rhi::ResourceHandle capture_min_depth_buffer_{ rhi::handles::INVALID_RESOURCE };
    // T3.3 (D10): indirect trace/resolve wiring — params UBOs, per-frame
    // indirect_out target and a 16B dummy for the unused GlobalShaderData slot.
    rhi::ResourceHandle indirect_trace_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_resolve_params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_out_tex_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle indirect_dummy_cb_{ rhi::handles::INVALID_RESOURCE };
    // T3.3: atlas-space directional visibility (float per atlas texel,
    // CPU-initialized to 1.0) + triple-buffered shadow params CB.
    rhi::ResourceHandle shadow_visibility_buffer_{ rhi::handles::INVALID_RESOURCE };
    bool s_ab_readback_done_ = false;
    bool s_albedo_readback_done_ = false;
    rhi::ResourceHandle sc_offline_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    math::v3 sc_offline_sdf_origin_{};
    math::v3 sc_offline_sdf_extent_{};
    u32 sc_offline_sdf_resolution_ = 0;
    rhi::ResourceHandle shadow_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    u32 atlas_size_ = 0;
    u32 page_size_ = 0;
    u32 light_eval_rr_cursor_ = 0;   // T1.4: round-robin card cursor for page-budget LightEval
    u32 lighting_atlas_seed_mask_ = 0; // one bit per triple-buffered lighting slot
    GITemporalStrategy temporal_strategy_ = GITemporalStrategy::EMA;   // X2/A3 枚举存储（暂不改变行为）

    // GBuffer-gather capture state
    rhi::ResourceHandle capture_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // CardFill (SDF-based fill) state
    rhi::ResourceHandle card_fill_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    bool atlas_seeded_ = false;   // AtlasInit runs once; Capture overwrites per frame
};

} // namespace primal::graphics::lumen
