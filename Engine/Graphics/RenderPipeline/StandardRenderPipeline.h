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
#include "Graphics/Volume/VolumePass.h"
#include "Graphics/Volume/VolumeRenderer.h"
#include "Graphics/Volume/FroxelFogPass.h"
#include "Graphics/Fluid/FluidRenderPass.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/RenderPipeline/Modules/ForwardSceneRenderer.h"
#include "Graphics/RenderPipeline/Modules/ShadowMapModule.h"
#include "Graphics/RenderPipeline/Modules/DeferredLightingModule.h"
#include "Graphics/RenderPipeline/Modules/FinalBlitModule.h"
#include "Graphics/RenderPipeline/Modules/GIGatherModule.h"
#include "Graphics/RenderPipeline/Modules/FusionCompositeModule.h"
#include "Graphics/RenderPipeline/Modules/SCDDGIIntegrationModule.h"
#include "Graphics/Nanite/DepthHistoryManager.h"
#include "Graphics/Nanite/ColorHistoryManager.h"
#include "Graphics/Nanite/HZBSystem.h"
#include "Graphics/PCG/PCGSDFReadbackManager.h"
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

    // --- Runtime pass configuration ---
    void SetPassEnabled(RenderPassID pass, bool enabled);
    bool IsPassEnabled(RenderPassID pass) const;
    bool IsPassActive(RenderPassID pass) const;
    static constexpr u32 GetPassCount() { return static_cast<u32>(RenderPassID::Count); }

    // --- Runtime settings ---
    const RenderPipelineSettings& GetSettings() const { return settings_; }
    void UpdateSettings(const RenderPipelineSettings& settings);

    lumen::StaticProbeVolume* GetStaticProbeVolume() const { return static_probe_volume_.get(); }

    // PCG SDF readback — provides real GlobalSDF data for PCG scatter
    pcg::PCGSDFReadbackManager* GetPCGSDFReadback() { return &pcg_sdf_readback_; }
    const pcg::PCGSDFReadbackManager* GetPCGSDFReadback() const { return &pcg_sdf_readback_; }

    /// Set logical viewport dimensions.
    /// Actual render resolution = logical × quality_config_.render_scale.
    void SetViewportSize(u32 width, u32 height) {
        logical_width_ = width;
        logical_height_ = height;
        render_width_  = static_cast<u32>(width  * settings_.quality.render_scale);
        render_height_ = static_cast<u32>(height * settings_.quality.render_scale);
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

    rhi::RHIDeviceBase* GetDevice() const { return device_; }

    // --- Editor mode (lightweight forward rendering) ---
    void SetEditorMode(bool enabled) { editor_mode_ = enabled; }
    bool IsEditorMode() const { return editor_mode_; }
    bool LoadEditorScene(const std::string& model_path) {
        if (!forward_renderer_) return false;
        return forward_renderer_->LoadScene(model_path);
    }
    ForwardSceneRenderer* GetForwardRenderer() const { return forward_renderer_.get(); }

    // --- PCG Entity integration (Phase 3c/3d) ---
    void SetPCGEntities(std::vector<id::id_type> entity_ids,
                        std::vector<u32> mesh_slot_indices) {
        pcg_entity_ids_ = std::move(entity_ids);
        pcg_mesh_slot_indices_ = std::move(mesh_slot_indices);
    }
    void ClearPCGEntities() {
        pcg_entity_ids_.clear();
        pcg_mesh_slot_indices_.clear();
    }

    // --- Static scene Entity integration (Phase 3e) ---
    void SetStaticEntities(std::vector<id::id_type> entity_ids,
                           std::vector<u32> mesh_slot_indices) {
        static_entity_ids_ = std::move(entity_ids);
        static_mesh_slot_indices_ = std::move(mesh_slot_indices);
    }
    void ClearStaticEntities() {
        static_entity_ids_.clear();
        static_mesh_slot_indices_.clear();
    }

    // --- Geometry entity integration (Phase 3.5) ---
    void SetGeometryEntities(std::vector<id::id_type> entity_ids) {
        geometry_entity_ids_ = std::move(entity_ids);
    }
    void ClearGeometryEntities() {
        geometry_entity_ids_.clear();
    }

    // --- Content system entity registration (Phase 4) ---
    // Register a mesh resource + create ECS entity → enters render pipeline
    // geometry_content_id: content system mesh ID (from create_resource)
    // texture_content_ids: [albedo, normal, ORM] content IDs (invalid_id for fallback)
    // Returns entity ID, or invalid_id on failure
    id::id_type RegisterMeshEntity(id::id_type geometry_content_id,
                                   const id::id_type* texture_content_ids,
                                   u32 texture_count);
    void UnregisterMeshEntity(id::id_type entity_id);

    /// Hot-reload a shader. Returns true on success.
    bool ReloadShader(rhi::ShaderHandle shader, const void* data, u32 size) override;

    // Access the RenderScene currently being rendered. Returns nullptr if
    // called outside Render(). Used by C ABI (EngineDLL/RenderPipelineAPI.cpp)
    // entry points that need to mutate the scene outside the render call.
    RenderScene* GetCurrentScene() { return current_scene_; }

private:
    void InitializeSubsystems();
    void InitializeLumenPasses();
    void ShutdownSubsystems();
    void ShutdownLumenPasses();

    void UpdatePerFrame(RenderScene& scene, RenderView& view);
    void BuildRenderGraph(rhi::ResourceHandle backBuffer, u32 currentBufferIndex);
    bool ApplyConfigChanges();

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

    // Unified settings (replaces lumen_config_ + quality_config_ + hardcoded values)
    RenderPipelineSettings settings_{};
    bool settings_dirty_ = false;

    // --- Nanite subsystems ---
    nanite::GPUCullingPipeline* culling_pipeline_ = nullptr;
    nanite::NaniteStreamingManager* streaming_manager_ = nullptr;
    std::unique_ptr<nanite::DepthHistoryManager> depth_history_;
    std::unique_ptr<nanite::ColorHistoryManager> color_history_;
    std::unique_ptr<nanite::HZBSystem> hzb_system_;

    // --- Scene snapshot ---
    std::unique_ptr<RenderSceneSnapshot> scene_snapshot_;

    // Cached RenderScene pointer — valid between Render() entry and the next
    // Render() call. Set at the top of Render() / RenderWithCommandBuffer().
    // Used by C ABI entry points (PipelineRegisterStreamingMeshEntity etc.)
    // that need to mutate the scene outside the render call.
    RenderScene* current_scene_{nullptr};

    // --- Lumen GI ---
    std::unique_ptr<lumen::LumenDDGIPass> ddgi_pass_;
    std::unique_ptr<lumen::LumenSSAOPass> ssao_pass_;
    std::unique_ptr<lumen::LumenSSGIPass> ssgi_pass_;
    std::unique_ptr<lumen::SurfaceCachePass> surface_cache_pass_;
    std::unique_ptr<lumen::ScreenProbeGIPass> screen_probe_pass_;
    std::unique_ptr<lumen::StaticProbeVolume> static_probe_volume_;

    // --- Volume Rendering ---
    std::unique_ptr<volume::VolumePass> volume_pass_;
    std::unique_ptr<volume::VolumeRenderer> volume_renderer_;
    std::unique_ptr<volume::FroxelFogPass> froxel_fog_pass_;

    // --- Fluid Rendering ---
    std::unique_ptr<fluid::FluidRenderPass> fluid_render_pass_;

    // --- Forward Renderer (Editor mode) ---
    std::unique_ptr<ForwardSceneRenderer> forward_renderer_;
    bool editor_mode_{false};

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

    // --- PCG SDF readback ---
    pcg::PCGSDFReadbackManager pcg_sdf_readback_;
    bool pcg_sdf_readback_initialized_{false};

    // --- PCG Entity → RenderScene bridge (Phase 3c) ---
    std::vector<id::id_type> pcg_entity_ids_;
    std::vector<u32> pcg_mesh_slot_indices_;

    // --- Static Entity → RenderScene bridge (Phase 3e) ---
    std::vector<id::id_type> static_entity_ids_;
    std::vector<u32> static_mesh_slot_indices_;

    // --- Geometry Entity bridge (Phase 3.5) ---
    std::vector<id::id_type> geometry_entity_ids_;

    void SyncEntitiesToRenderScene(RenderScene& scene);

    // --- Persistent render state ---
    math::m4x4 view_matrix_{};
    math::m4x4 proj_matrix_{};
    math::v3 camera_position_{};

    // Per-frame data
    u64 frameCount_{0};
    bool subsystems_initialized_{false};

    // Convenience accessors for frequently used settings fields
    const PipelineQualityConfig& qc() const { return settings_.quality; }
    const lumen::LumenConfig& lc() const { return settings_.lumen; }
};

} // namespace primal::graphics
