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
#include <chrono>

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
     * @details 自动管理帧同步和渲染流程
     * @param scene 渲染场景
     * @param view 渲染视图
     */
    void Render(RenderScene& scene, RenderView& view);

    /**
     * @brief 获取当前帧索引 (0 to MAX_FRAMES_IN_FLIGHT-1)
     * @return 当前帧索引
     */
    uint32_t GetCurrentFrameIndex() const { return currentFrameIndex_; }

    /**
     * @brief 注册材质实例（临时，用于查找）
     * @param id 材质ID
     * @param materialInstance 材质实例指针
     */
    void RegisterMaterialInstance(id::id_type id, MaterialInstance* materialInstance);

    /**
     * @brief 获取材质实例
     * @param id 材质ID
     * @return 材质实例指针，若不存在返回nullptr
     */
    MaterialInstance* GetMaterialInstance(id::id_type id) const;

    /**
     * @brief 获取当前帧索引 (0 to MAX_FRAMES_IN_FLIGHT-1)
     */
    /**
     * @brief 开始新的一帧
     * @details 获取下一个可用的后台缓冲区
     * @param outBackBuffer 输出后台缓冲区纹理句柄
     * @param outSignalFence 输出用于同步的Fence句柄，Pipeline必须在提交渲染命令时Signal此Fence
     * @return 是否成功开始新帧
     */
    bool BeginFrame(rhi::ResourceHandle& outBackBuffer, rhi::SyncHandle& outSignalFence);

    /**
     * @brief 结束当前帧
     * @details 提交渲染结果并呈现
     */
    void EndFrame();

    /**
     * @brief 获取当前后台缓冲区的描述信息
     * @return 纹理描述
     */
    rhi::TextureDesc GetBackBufferDesc() const;

    /**
     * @brief 处理窗口大小改变
     * @param width 新宽度
     * @param height 新高度
     */
    void Resize(uint32_t width, uint32_t height);

    ForwardRenderer& GetRenderer() { return forwardRenderer_; }

private:
    /**
     * @brief 等待上一帧完成 (CPU wait)
     * @param frameIndex 当前帧索引
     */
    void Wait(uint32_t frameIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    rhi::RHISwapChain* swapChain_{nullptr};
    
    // Multi-buffered command buffers
    utl::vector<rhi::CommandBufferHandle> cmdBufferHandles_;
    utl::vector<rhi::RHICommandBuffer*> cmdBuffers_;
    utl::vector<rhi::SyncHandle> frameFences_; // CPU-GPU sync fences
    uint32_t currentFrameIndex_{0};
    uint32_t currentImageIndex_{0}; // Index of the swapchain image acquired for the current frame

    rhi::ResourceHandle depthStencilTexture_{rhi::handles::INVALID_RESOURCE};
    std::unordered_map<id::id_type, MaterialInstance*> materialInstances_;

    ForwardRenderer forwardRenderer_;
};

} // namespace primal::graphics
