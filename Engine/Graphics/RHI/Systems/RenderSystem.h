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

#include "Graphics/RHI/Core/RHISystem.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"

namespace primal::graphics {

struct RenderSystemInitInfo {
    rhi::RHIDeviceBase* device{nullptr};
    rhi::RHIEntityManager* entityManager{nullptr};
    platform::window_handle window{nullptr};
    u32 width{0};
    u32 height{0};
};

/**
 * @brief 渲染系统核心类
 * @details 负责每帧的渲染逻辑调度，包括场景剔除、DrawPacket生成和提交
 */
class RenderSystem : public rhi::RHISystem {
public:
    RenderSystem();
    ~RenderSystem() override;

    /**
     * @brief 初始化渲染系统
     * @param info 初始化信息
     * @return 初始化是否成功
     */
    bool Initialize(const RenderSystemInitInfo& info);

    using RHISystem::Initialize;

    // RHISystem 接口实现
    void Update(float deltaTime) override;
    void Render(rhi::RHICommandBuffer* cmdBuffer) override;

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
    u32 GetCurrentFrameIndex() const { return currentFrameIndex_; }

    /**
     * @brief 获取当前 acquired swapchain image 的索引
     * @details T4.6.5 part 30.13 (X5 fix): caller 需要 per-image semaphore
     *          来避免 render-done semaphore 重用。RenderSystem 内部有
     *          currentImageIndex_,这里暴露给外部使用。
     */
    u32 GetCurrentImageIndex() const { return currentImageIndex_; }

    /**
     * @brief 获取当前帧的 Command Buffer
     * @return 当前帧的 Command Buffer 指针
     */
    rhi::RHICommandBuffer* GetCurrentCommandBuffer() { return cmdBuffers_[currentFrameIndex_]; }

    /**
     * @brief 获取当前帧的 Command Buffer 句柄
     * @return 当前帧的 Command Buffer 句柄
     */
    rhi::CommandBufferHandle GetCurrentCommandBufferHandle() { return cmdBufferHandles_[currentFrameIndex_]; }

    /**
     * @brief 注册材质实例
     * @param id 材质ID
     * @param materialInstance 材质实例指针 (共享所有权)
     */
    void RegisterMaterialInstance(id::id_type id, std::shared_ptr<MaterialInstance> materialInstance);

    /**
     * @brief 获取材质实例
     * @param id 材质ID
     * @return 材质实例指针，若不存在返回nullptr
     */
    std::shared_ptr<MaterialInstance> GetMaterialInstance(id::id_type id) const;

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
     * @brief 结束当前帧 (T4.6.5 part 30.12: GPU-GPU 同步重载)
     * @details Present 等待 render-done semaphore,确保上一帧的 draw 完成后才 present
     * @param renderDoneSemaphore Submit 信号给的 render-done 信号量
     */
    void EndFrame(rhi::SyncHandle renderDoneSemaphore);

    /**
     * @brief 获取当前帧的 fence (CPU-GPU 同步)
     * @details 外部调用 Submit 时可能需要传入 fence,此接口返回当前帧的 fence
     * @param frameIndex 帧索引 (内部对 MAX_FRAMES_IN_FLIGHT 取模)
     * @return SyncHandle,如果未初始化返回 INVALID_SYNC
     */
    rhi::SyncHandle GetFrameFence(u32 frameIndex) const {
        if (frameFences_.empty()) return rhi::handles::INVALID_SYNC;
        return frameFences_[frameIndex % rhi::MAX_FRAMES_IN_FLIGHT];
    }

    /**
     * @brief 获取当前帧 acquire→draw 的 GPU-GPU 信号量
     * @details T4.6.5 part 30.14 (X1 follow-up): BeginFrame 内部调用
     *          AcquireNextImage(imageSemaphores_[currentFrameIndex_]),这个
     *          semaphore 由 acquire 信号、由 caller 的 Submit 等待。原本
     *          BeginFrame 没有把这个 semaphore 暴露出来,caller 只能拿到 fence
     *          (CPU-GPU),导致 Submit 传 waitSemaphore=garbage 触发
     *          VUID-vkQueueSubmit-pWaitSemaphores-03238。
     */
    rhi::SyncHandle GetCurrentImageAvailableSemaphore() const {
        if (imageSemaphores_.empty()) return rhi::handles::INVALID_SYNC;
        return imageSemaphores_[currentFrameIndex_];
    }

    /**
     * @brief 获取当前后台缓冲区的描述信息
     * @return 纹理描述
     */
    rhi::TextureDesc GetBackBufferDesc() const;

    /**
     * @brief 设置当前渲染使用的 RenderPass
     * @details 用于 Material::GetPipeline 获取正确的 Pipeline
     * @param renderPass RenderPass 句柄
     */
    void SetCurrentRenderPass(rhi::RenderPassHandle renderPass) { currentRenderPass_ = renderPass; }

    /**
     * @brief 处理窗口大小改变
     * @param width 新宽度
     * @param height 新高度
     */
    void Resize(u32 width, u32 height);

    ForwardRenderer& GetRenderer() { return forwardRenderer_; }

private:
    /**
     * @brief 等待上一帧完成 (CPU wait)
     * @param frameIndex 当前帧索引
     */
    void Wait(u32 frameIndex);

    rhi::RHIDeviceBase* device_{nullptr};
    rhi::RHISwapChain* swapChain_{nullptr};
    
    // Multi-buffered command buffers
    utl::vector<rhi::CommandBufferHandle> cmdBufferHandles_;
    utl::vector<rhi::RHICommandBuffer*> cmdBuffers_;
    utl::vector<rhi::SyncHandle> frameFences_; // CPU-GPU sync fences
    utl::vector<rhi::SyncHandle> imageSemaphores_; // T4.6.5 part 30.5: GPU-GPU acquire→draw semaphores
    u32 currentFrameIndex_{0};
    u32 currentImageIndex_{0}; // Index of the swapchain image acquired for the current frame

    rhi::ResourceHandle depthStencilTexture_{rhi::handles::INVALID_RESOURCE};
    std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>> materialInstances_;

    ForwardRenderer forwardRenderer_;

    // 当前渲染使用的 RenderPass
    rhi::RenderPassHandle currentRenderPass_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics
