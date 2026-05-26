#pragma once

#include "RenderPipeline.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHIGPUOptimizer.h"
#include "Graphics/Lumen/SurfaceCache/SurfaceCachePass.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include "Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Graphics/Lumen/LumenTypes.h"
#include <memory>

namespace primal::graphics {

class StandardRenderPipeline : public RenderPipeline {
public:
    StandardRenderPipeline() = default;
    ~StandardRenderPipeline() override;

    bool Initialize(rhi::RHIDeviceBase* device) override;
    void Shutdown() override;
    void Render(RenderScene& scene, RenderView& view, rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc, rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC) override;

    /**
     * @brief Set an override output resource (e.g. for offscreen testing)
     * @param handle Resource Handle
     * @param desc Texture Description
     */
    void SetOutputResource(rhi::ResourceHandle handle, const rhi::TextureDesc& desc) {
        outputResource_ = handle;
        outputDesc_ = desc;
    }

    /**
     * @brief Configure Lumen GI quality and settings
     */
    void SetLumenConfig(const lumen::LumenConfig& config) {
        lumen_config_ = config;
    }

    /**
     * @brief Get pipeline statistics
     */
    const PipelineStatistics& GetStats() const { return stats_; }

    /**
     * @brief Access the static probe volume (baked GI data)
     */
    lumen::StaticProbeVolume* GetStaticProbeVolume() const { return static_probe_volume_.get(); }

private:
    void SetupGraph(RenderScene& scene, RenderView& view);
    void InitializeLumenPasses();
    void ShutdownLumenPasses();

    rhi::RHIDeviceBase* device_{nullptr};
    std::unique_ptr<rendergraph::RenderGraph> renderGraph_;
    std::unique_ptr<rhi::RHIGPUOptimizer> gpuOptimizer_;

    rhi::ResourceHandle outputResource_{rhi::handles::INVALID_RESOURCE};
    rhi::TextureDesc outputDesc_;

    PipelineStatistics stats_;

    // Lumen GI
    lumen::LumenConfig lumen_config_{};
    std::unique_ptr<lumen::SurfaceCachePass> surface_cache_pass_;
    std::unique_ptr<lumen::ScreenProbeGIPass> screen_probe_pass_;
    std::unique_ptr<lumen::StaticProbeVolume> static_probe_volume_;

    // Per-frame data
    u64 frameCount_{0};
};

} // namespace primal::graphics
