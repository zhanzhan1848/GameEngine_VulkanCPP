#pragma once

#include "RenderPipeline.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHIGPUOptimizer.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/Lumen/DDGI/LumenDDGIPass.h"
#include "Graphics/Lumen/SSAO/LumenSSAOPass.h"
#include "Graphics/Lumen/SSGI/LumenSSGIPass.h"
#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/RenderPipeline/Modules/ShadowMapModule.h"
#include "Graphics/RenderPipeline/Modules/DeferredLightingModule.h"
#include "Graphics/RenderPipeline/Modules/FinalBlitModule.h"
#include "Graphics/RenderPipeline/Modules/GIGatherModule.h"
#include "Graphics/RenderPipeline/Modules/FusionCompositeModule.h"
#include "Graphics/RenderPipeline/Modules/SCDDGIIntegrationModule.h"
#include "Graphics/Nanite/DepthHistoryManager.h"
#include "Graphics/Nanite/ColorHistoryManager.h"
#include "Graphics/Nanite/HZBSystem.h"
#include <memory>

namespace primal::graphics {

namespace nanite {
class GPUCullingPipeline;
class NaniteStreamingManager;
}

class StandardRenderPipeline : public RenderPipeline {
public:
    StandardRenderPipeline() = default;
    ~StandardRenderPipeline() override;

    bool Initialize(rhi::RHIDeviceBase* device) override;
    void Shutdown() override;
    void Render(RenderScene& scene, RenderView& view, rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc, rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC) override;

    /// Render using a pre-allocated command buffer (matches TestNaniteStreamingPipeline pattern).
    /// The caller manages CB lifecycle: Reset() + Begin() before, End() + Submit() after.
    void RenderWithCommandBuffer(RenderScene& scene, RenderView& view,
                                  rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc,
                                  rhi::RHICommandBuffer* cmd, u32 bufferIndex,
                                  rhi::CommandBufferHandle cmdHandle,
                                  rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC);

    void SetOutputResource(rhi::ResourceHandle handle, const rhi::TextureDesc& desc) {
        outputResource_ = handle;
        outputDesc_ = desc;
    }

    void SetLumenConfig(const lumen::LumenConfig& config);

    const PipelineStatistics& GetStats() const { return stats_; }

    /// Override specific quality flags. Call AFTER SetLumenConfig — triggers re-init of Lumen passes.
    void SetQualityOverride(bool enable_screen_probes, bool enable_surface_cache = false);

    lumen::StaticProbeVolume* GetStaticProbeVolume() const { return static_probe_volume_.get(); }

    /// Set logical viewport dimensions.
    /// Actual render resolution = logical × quality_config_.render_scale.
    void SetViewportSize(u32 width, u32 height) {
        logical_width_ = width;
        logical_height_ = height;
        render_width_  = static_cast<u32>(width  * quality_config_.render_scale);
        render_height_ = static_cast<u32>(height * quality_config_.render_scale);
    }

    /// Inject pre-compiled shader handles. Call before SetLumenConfig.
    struct ShaderHandles {
        rhi::ShaderHandle deferred_vs{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle deferred_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle blit_vs{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle blit_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle shadow_filter{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle gi_gather{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle fusion_indirect_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle fusion_composite_ps{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle sc_card_radiance{rhi::handles::INVALID_SHADER};
        rhi::ShaderHandle sc_probe_irradiance{rhi::handles::INVALID_SHADER};
    };

    void SetShaderHandles(const ShaderHandles& handles) {
        deferred_vs_ = handles.deferred_vs;
        deferred_ps_ = handles.deferred_ps;
        blit_vs_ = handles.blit_vs;
        blit_ps_ = handles.blit_ps;
        shadow_filter_shader_ = handles.shadow_filter;
        gi_gather_shader_ = handles.gi_gather;
        fusion_indirect_ps_ = handles.fusion_indirect_ps;
        fusion_composite_ps_ = handles.fusion_composite_ps;
        sc_card_radiance_shader_ = handles.sc_card_radiance;
        sc_probe_irradiance_shader_ = handles.sc_probe_irradiance;
    }

    lumen::SurfaceCachePass* GetSurfaceCachePass() const {
        return surface_cache_pass_ ? surface_cache_pass_.get() : nullptr;
    }

private:
    void InitializeSubsystems();
    void InitializeLumenPasses();
    void ShutdownSubsystems();
    void ShutdownLumenPasses();

    void UpdatePerFrame(RenderScene& scene, RenderView& view);
    void BuildRenderGraph(rhi::ResourceHandle backBuffer, u32 currentBufferIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    std::unique_ptr<rendergraph::RenderGraph> renderGraph_;
    std::unique_ptr<rhi::RHIGPUOptimizer> gpuOptimizer_;

    rhi::ResourceHandle outputResource_{rhi::handles::INVALID_RESOURCE};
    rhi::TextureDesc outputDesc_;

    PipelineStatistics stats_;

    // Viewport
    u32 logical_width_ = 1280;   // window logical size (set via SetViewportSize)
    u32 logical_height_ = 720;
    u32 render_width_ = 1280;    // actual GPU render resolution (= logical × render_scale)
    u32 render_height_ = 720;
    u32 target_width_ = 0;       // backbuffer size (may differ on Retina/HiDPI)
    u32 target_height_ = 0;

    // Quality config (derived from LumenConfig + PipelineQualityConfig)
    lumen::LumenConfig lumen_config_{};
    PipelineQualityConfig quality_config_{};

    // --- Nanite subsystems ---
    nanite::GPUCullingPipeline* culling_pipeline_ = nullptr;
    nanite::NaniteStreamingManager* streaming_manager_ = nullptr;
    std::unique_ptr<nanite::DepthHistoryManager> depth_history_;
    std::unique_ptr<nanite::ColorHistoryManager> color_history_;
    std::unique_ptr<nanite::HZBSystem> hzb_system_;

    // --- Scene snapshot ---
    std::unique_ptr<RenderSceneSnapshot> scene_snapshot_;

    // --- Lumen GI ---
    std::unique_ptr<lumen::LumenDDGIPass> ddgi_pass_;
    std::unique_ptr<lumen::LumenSSAOPass> ssao_pass_;
    std::unique_ptr<lumen::LumenSSGIPass> ssgi_pass_;
    std::unique_ptr<lumen::SurfaceCachePass> surface_cache_pass_;
    std::unique_ptr<lumen::ScreenProbeGIPass> screen_probe_pass_;
    std::unique_ptr<lumen::StaticProbeVolume> static_probe_volume_;

    // --- Pipeline modules ---
    std::unique_ptr<ShadowMapModule> shadow_module_;
    std::unique_ptr<DeferredLightingModule> deferred_module_;
    std::unique_ptr<FinalBlitModule> final_blit_module_;
    std::unique_ptr<GIGatherModule> gi_gather_module_;
    std::unique_ptr<FusionCompositeModule> fusion_module_;
    std::unique_ptr<SCDDGIIntegrationModule> sc_ddgi_module_;

    // --- Shader compilation cache ---
    rhi::ShaderHandle shadow_filter_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle deferred_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle deferred_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_vs_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle blit_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle gi_gather_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle fusion_indirect_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle fusion_composite_ps_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle sc_card_radiance_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle sc_probe_irradiance_shader_{rhi::handles::INVALID_SHADER};

    // --- Utility textures ---
    rhi::ResourceHandle black_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle white_texture_{rhi::handles::INVALID_RESOURCE};

    // --- Persistent render state ---
    math::m4x4 view_matrix_{};
    math::m4x4 proj_matrix_{};
    math::v3 camera_position_{};
    math::v4 light_pos_{-0.537f, -0.894f, 0.476f, 0.0f}; // Normalized "to light" direction
    math::v4 light_color_{5.0f, 5.0f, 5.0f, 1.0f};

    // Per-frame data
    u64 frameCount_{0};
    bool subsystems_initialized_{false};
};

} // namespace primal::graphics
