#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include <unordered_map>
#include <string>

namespace primal::graphics {

struct PipelineStatistics {
    double cpuFrameTimeMs{0.0};
    double gpuFrameTimeMs{0.0};
    u32 drawCallCount{0};
    u32 triangleCount{0};
    
    // Per-pass GPU execution time in milliseconds
    std::unordered_map<std::string, double> passExecutionTimes;
};

class RenderPipeline {
public:
    static RenderPipeline* Get() { return s_instance; }

    virtual ~RenderPipeline() = default;

    /**
     * @brief Initialize the render pipeline
     * @param device The RHI device to use
     * @return True if initialization was successful
     */
    virtual bool Initialize(rhi::RHIDeviceBase* device) = 0;

    /**
     * @brief Shutdown the render pipeline and release resources
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Render a scene from a specific view to a target resource
     * @param scene The scene to render
     * @param view The view parameters (camera, viewport, etc.)
     * @param target The target texture to render into
     * @param targetDesc The description of the target texture
     * @param signalFence Optional fence to signal when rendering completes
     */
    virtual void Render(RenderScene& scene, RenderView& view, rhi::ResourceHandle target, const rhi::TextureDesc& targetDesc, rhi::SyncHandle signalFence = rhi::handles::INVALID_SYNC) = 0;

    virtual PipelineStatistics GetStatistics() const { return {}; }

    virtual bool ReloadShader(rhi::ShaderHandle shader, const void* data, u32 size) {
        (void)shader; (void)data; (void)size;
        return false;
    }

protected:
    static RenderPipeline* s_instance;
};

} // namespace primal::graphics

#define g_RenderPipeline primal::graphics::RenderPipeline::Get()
