/**
 * @file MetalSync.h
 * @brief Metal 同步原语实现
 * @details 封装 MTLSharedEvent 和 MTLFence 实现 GPU-CPU 和 GPU-GPU 同步
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief Metal 同步对象
 * @details 封装 MTL::SharedEvent 用于 CPU-GPU 和 GPU-GPU 同步
 */
class MetalSync {
public:
    /**
     * @brief 构造函数
     * @param device Metal 设备指针
     */
    explicit MetalSync(MTL::Device* device);

    /**
     * @brief 析构函数
     */
    ~MetalSync();

    /**
     * @brief 获取原生 SharedEvent 对象
     */
    MTL::SharedEvent* GetNativeEvent() const { return event_; }

    /**
     * @brief 获取当前信号值
     */
    uint64_t GetValue() const;

    /**
     * @brief 设置信号值 (CPU 端)
     */
    void SetValue(uint64_t value);

private:
    MTL::SharedEvent* event_{nullptr}; ///< Metal 共享事件对象
};

} // namespace primal::graphics::rhi
