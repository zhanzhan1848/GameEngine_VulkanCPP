/**
 * @file RHISwapChain.h
 * @brief RHI交换链基类
 * @details 定义交换链接口，用于管理呈现和后台缓冲区
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "RHIResource.h"
#include "RHITypes.h"

namespace primal::graphics::rhi {

class RHIDeviceBase;

/**
 * @brief RHI交换链基类
 * @details 管理呈现队列和后台缓冲区，负责将渲染结果呈现到屏幕
 */
class RHISwapChain : public RHIResource {
public:
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param desc 交换链描述符
     */
    RHISwapChain(RHIDeviceBase& device, const SwapChainDesc& desc);

    /**
     * @brief 虚析构函数
     */
    virtual ~RHISwapChain() override = default;

    // === RHIResource 接口实现 ===
    
    /**
     * @brief 初始化交换链
     * @return 初始化是否成功
     */
    virtual bool Initialize() override = 0;

    /**
     * @brief 销毁交换链
     */
    virtual void Destroy() override;

    // === 交换链特定接口 ===

    /**
     * @brief 获取下一个图像索引
     * @param imageIndex 输出图像索引
     * @param semaphore 信号量
     * @param fence 栅栏
     * @return 是否成功
     */
    virtual bool AcquireNextImage(uint32_t* imageIndex, SyncHandle semaphore = handles::INVALID_SYNC, SyncHandle fence = handles::INVALID_SYNC) = 0;

    /**
     * @brief 呈现当前后台缓冲区
     * @details 将渲染完成的图像呈现到屏幕
     * @param semaphore 等待的信号量
     */
    virtual void Present(SyncHandle semaphore) = 0;

    /**
     * @brief 调整交换链大小
     * @param width 新宽度
     * @param height 新高度
     */
    virtual void Resize(uint32_t width, uint32_t height);

    /**
     * @brief 获取当前后台缓冲区索引
     * @return 当前正在使用的后台缓冲区索引
     */
    virtual uint32_t GetCurrentBackBufferIndex() const = 0;

    /**
     * @brief 获取后台缓冲区纹理句柄
     * @param index 缓冲区索引
     * @return 纹理句柄，如果索引无效返回INVALID_RESOURCE
     */
    virtual ResourceHandle GetBackBuffer(uint32_t index) const = 0;

    /**
     * @brief 获取交换链描述符
     * @return 交换链描述符引用
     */
    const SwapChainDesc& GetDesc() const { return swapChainDesc_; }

protected:
    SwapChainDesc swapChainDesc_; ///< 交换链描述符
};

} // namespace primal::graphics::rhi
