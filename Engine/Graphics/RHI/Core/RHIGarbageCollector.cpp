/**
 * @file RHIGarbageCollector.cpp
 * @brief RHI垃圾回收器实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-09
 * @version 0.1.0
 */

#include "RHIGarbageCollector.h"
#include <iostream>

namespace primal::graphics::rhi {

RHIGarbageCollector::~RHIGarbageCollector() {
    Shutdown();
}

void RHIGarbageCollector::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    garbageQueue_.clear();
    currentFrame_ = 0;
}

void RHIGarbageCollector::Shutdown() {
    Flush();
}

void RHIGarbageCollector::Flush() {
    std::deque<GarbageItem> currentQueue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(currentQueue, garbageQueue_);
    }
    
    if (!currentQueue.empty()) {
        // std::cout << "[GC] Flushing " << currentQueue.size() << " items" << std::endl;
        for (const auto& item : currentQueue) {
            if (item.callback) {
                item.callback();
            }
        }
    }

    // 如果在回调过程中产生了新的垃圾（例如CommandBuffer销毁时会销毁关联的Sync对象），需要递归清理
    bool hasMore = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hasMore = !garbageQueue_.empty();
    }
    if (hasMore) {
        Flush();
    }
}

void RHIGarbageCollector::DeferredDestroy(std::function<void()>&& callback) {
    if (!callback) return;

    std::lock_guard<std::mutex> lock(mutex_);
    // std::cout << "[GC] DeferredDestroy queued frame=" << currentFrame_ << std::endl;
    garbageQueue_.push_back({currentFrame_, std::move(callback)});
}

void RHIGarbageCollector::Update(uint64_t completedFrame, double maxDurationMs) {
    std::vector<std::function<void()>> callbacksToRun;
    
    // 1. Collect all valid candidates
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 移除所有 frameIndex <= completedFrame 的项目
        // 队列是按时间顺序排列的，所以只需检查头部
        while (!garbageQueue_.empty()) {
            const auto& item = garbageQueue_.front();
            
            // 如果项目的帧索引小于等于已完成的帧索引，说明可以安全销毁
            if (item.frameIndex <= completedFrame) {
                if (item.callback) {
                    // std::cout << "[GC] Queuing callback for frame " << item.frameIndex << std::endl;
                    callbacksToRun.push_back(item.callback);
                }
                garbageQueue_.pop_front();
            } else {
                // 遇到第一个未完成的帧，后面的肯定也未完成（因为是按序插入）
                break;
            }
        }
    }

    // 2. Execute with time budget
    if (!callbacksToRun.empty()) {
        auto startTime = std::chrono::high_resolution_clock::now();
        size_t executedCount = 0;
        
        for (const auto& callback : callbacksToRun) {
            callback();
            executedCount++;

            // Check budget if enabled
            if (maxDurationMs > 0.0) {
                auto now = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> elapsed = now - startTime;
                if (elapsed.count() >= maxDurationMs) {
                    // Re-queue remaining callbacks
                    if (executedCount < callbacksToRun.size()) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        // Push remaining items back to the FRONT of the queue to maintain order
                        // Note: Using push_front iterates in reverse to keep original order
                        for (size_t i = callbacksToRun.size(); i > executedCount; --i) {
                            // We use completedFrame as the frameIndex because these were already ripe for deletion
                            garbageQueue_.push_front({completedFrame, callbacksToRun[i-1]});
                        }
                    }
                    // std::cout << "[GC] Time limit reached (" << elapsed.count() << " ms). Executed " << executedCount << "/" << callbacksToRun.size() << " callbacks." << std::endl;
                    break;
                }
            }
        }
        
        if (maxDurationMs <= 0.0 || executedCount == callbacksToRun.size()) {
             // std::cout << "[GC] Executing " << executedCount << " callbacks for frame <= " << completedFrame << std::endl;
        }
    }
}

void RHIGarbageCollector::SetCurrentFrame(uint64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentFrame_ = frameIndex;
}

} // namespace primal::graphics::rhi
