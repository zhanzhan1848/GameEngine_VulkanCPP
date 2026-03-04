/**
 * @file RHIGarbageCollector.h
 * @brief RHI垃圾回收器
 * @details 管理GPU资源的延迟销毁，确保资源在GPU使用完毕后才被释放
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-09
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include <functional>
#include <mutex>
#include <deque>

namespace primal::graphics::rhi {

/**
 * @brief 垃圾回收器
 * @details 负责收集并执行延迟销毁回调
 */
class RHIGarbageCollector {
public:
    RHIGarbageCollector() = default;
    ~RHIGarbageCollector();

    /**
     * @brief 初始化
     */
    void Initialize();

    /**
     * @brief 关闭并清理所有剩余资源
     */
    void Shutdown();
    void Flush();

    /**
     * @brief 提交延迟销毁任务
     * @param callback 销毁回调函数
     */
    void DeferredDestroy(std::function<void()>&& callback);

    /**
     * @brief 更新GC状态，执行可回收资源的销毁
     * @param completedFrame GPU已完成的帧索引
     * @param maxDurationMs 最大执行时间(ms)，0表示不限制
     */
    void Update(u64 completedFrame, double maxDurationMs = 0.0);

    /**
     * @brief 设置当前CPU正在录制的帧索引
     * @param frameIndex 当前帧索引
     */
    void SetCurrentFrame(u64 frameIndex);

private:
    struct GarbageItem {
        u64 frameIndex;
        std::function<void()> callback;
    };

    std::mutex mutex_;
    std::deque<GarbageItem> garbageQueue_;
    u64 currentFrame_ = 0;
};

} // namespace primal::graphics::rhi
