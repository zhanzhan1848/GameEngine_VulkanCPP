/**
 * @file MetalSwapChain.h
 * @brief Metal 交换链实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "../../Core/RHISwapChain.h"
#include "MetalCommon.h"
#include "Utilities/Vector.h"

namespace primal::graphics::rhi {

class MetalDevice;

class MetalSwapChain : public RHISwapChain {
public:
    MetalSwapChain(MetalDevice& device, const SwapChainDesc& desc);
    ~MetalSwapChain() override;

    bool Initialize() override;
    void Destroy() override;
    /**
     * @brief 调整交换链大小
     * @param width 宽度
     * @param height 高度
     */
    void Resize(u32 width, u32 height) override;

    /**
     * @brief 获取下一个图像索引
     * @param imageIndex 输出图像索引
     * @param semaphore 信号量
     * @param fence 栅栏
     * @return 是否成功
     */
    bool AcquireNextImage(u32* imageIndex, SyncHandle semaphore = handles::INVALID_SYNC, SyncHandle fence = handles::INVALID_SYNC) override;

    /**
     * @brief 呈现画面
     * @param semaphore 等待的信号量
     */
    void Present(SyncHandle semaphore) override;
    u32 GetCurrentBackBufferIndex() const override;
    ResourceHandle GetBackBuffer(u32 index) const override;

    /**
     * @brief 获取当前帧的Drawable
     * @details 每次调用 currentDrawable 都会获取一个新的
     */
    MTL::Drawable* GetCurrentDrawable();

protected:
    // === RHIResource 接口实现 ===
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    MetalDevice& metalDevice_;
    MTK::View* mtkView_{nullptr};
    MTL::Drawable* currentDrawable_{nullptr};
    
    // 后台缓冲区句柄
    // Metal 不像 Vulkan 那样暴露固定的 SwapChain Image 列表
    // 但为了适配接口，我们维护一组 Handle，每一帧更新当前 Handle 对应的底层 Texture
    utl::vector<ResourceHandle> backBufferHandles_;
    
    u32 currentFrameIndex_{0};
};

} // namespace primal::graphics::rhi
