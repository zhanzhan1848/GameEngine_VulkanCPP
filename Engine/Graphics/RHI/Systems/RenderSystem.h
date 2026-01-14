#pragma once
#include "CommonHeaders.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/ForwardRenderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHISwapChain.h"
#include "Graphics/MaterialInstance.h"
#include "Platform/PlatformTypes.h"

namespace primal::graphics {

struct RenderSystemInitInfo {
    rhi::RHIDeviceBase* device{nullptr};
    platform::window_handle window{nullptr};
    uint32_t width{0};
    uint32_t height{0};
};

/**
 * @brief 渲染系统核心类
 * @details 负责每帧的渲染逻辑调度，包括场景剔除、DrawPacket生成和提交
 */
class RenderSystem {
public:
    RenderSystem();
    ~RenderSystem();

    /**
     * @brief 初始化渲染系统
     * @param info 初始化信息
     * @return 初始化是否成功
     */
    bool Initialize(const RenderSystemInitInfo& info);

    /**
     * @brief 关闭渲染系统
     */
    void Shutdown();

    /**
     * @brief 执行渲染
     * @param scene 渲染场景
     * @param view 渲染视图
     * @param frameIndex 当前帧索引 (for multi-buffering)
     */
    void Render(RenderScene& scene, RenderView& view, uint32_t frameIndex);

    /**
     * @brief 等待上一帧完成 (CPU wait)
     * @param frameIndex 当前帧索引
     */
    void Wait(uint32_t frameIndex);

    /**
     * @brief 注册材质实例（临时，用于查找）
     * @param id 材质ID
     * @param materialInstance 材质实例指针
     */
    void RegisterMaterialInstance(id::id_type id, MaterialInstance* materialInstance);

    /**
     * @brief 获取当前帧索引 (0 to MAX_FRAMES_IN_FLIGHT-1)
     */
    uint32_t GetCurrentFrameIndex() const { return currentFrameIndex_; }

private:
    rhi::RHIDeviceBase* device_{nullptr};
    rhi::RHISwapChain* swapChain_{nullptr};
    
    // Multi-buffered command buffers
    utl::vector<rhi::CommandBufferHandle> cmdBufferHandles_;
    utl::vector<rhi::RHICommandBuffer*> cmdBuffers_;
    utl::vector<rhi::SyncHandle> frameFences_; // CPU-GPU sync fences
    uint32_t currentFrameIndex_{0};

    rhi::ResourceHandle depthStencilTexture_{rhi::handles::INVALID_RESOURCE};
    std::unordered_map<id::id_type, MaterialInstance*> materialInstances_;

    ForwardRenderer forwardRenderer_;
};

} // namespace primal::graphics
