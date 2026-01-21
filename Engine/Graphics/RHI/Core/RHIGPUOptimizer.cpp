/**
 * @file RHIGPUOptimizer.cpp
 * @brief RHI GPU驱动渲染优化器实现
 * @details 提供智能GPU命令优化、同步管理、资源绑定优化和性能监控功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include "RHIGPUOptimizer.h"
#include "RHIBatchRenderer.h"
#include "RHIDeterministicPrefetch.h"
#include "RHIAdaptiveMemoryPool.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <fstream>
#include <iostream>

namespace primal::graphics::rhi {

// === 常量定义 ===
namespace constants {
    constexpr u32 MAX_CACHED_COMMAND_BUFFERS = 64;
    constexpr u32 MAX_RESOURCE_BINDINGS = 1024;
    constexpr u32 MAX_SYNC_POINTS = 32;
    constexpr u32 CACHE_EXPIRE_FRAMES = 300;  // 5秒 (60fps)
    constexpr f32 PERFORMANCE_SAMPLE_INTERVAL = 1.0f;  // 1秒
    constexpr f64 GPU_UTILIZATION_SMOOTHING = 0.1;     // 平滑因子
}

// === RHIGPUOptimizer 实现 ===

RHIGPUOptimizer::RHIGPUOptimizer(RHIDeviceBase& device, const OptimizationConfig& config)
    : device_(device), config_(config), currentFrameNumber_(0), 
      performanceUpdateCounter_(0), accumulatedFrameTime_(0.0), isInitialized_(false) {
    // 初始化统计缓冲区
    memset(statisticsBuffer_, 0, sizeof(statisticsBuffer_));
    
    // 预分配容器空间
    commandBufferCache_.reserve(constants::MAX_CACHED_COMMAND_BUFFERS);
    resourceBindingCache_.reserve(constants::MAX_RESOURCE_BINDINGS);
    syncPoints_.reserve(constants::MAX_SYNC_POINTS);
    pendingCommandBuffers_.reserve(config_.maxConcurrentCommandBuffers);
}

RHIGPUOptimizer::~RHIGPUOptimizer() {
    if (isInitialized_) {
        Shutdown();
    }
}

bool RHIGPUOptimizer::Initialize() {
    if (isInitialized_) {
        return true;
    }
    
    // 检查设备有效性
    if (!device_.IsValid()) {
        return false;
    }
    
    // 根据配置预分配资源
    if (config_.enableResourceBindingCache) {
        resourceBindingCache_.reserve(constants::MAX_RESOURCE_BINDINGS);
    }
    
    if (config_.maxConcurrentCommandBuffers > 0) {
        pendingCommandBuffers_.reserve(config_.maxConcurrentCommandBuffers);
    }
    
    syncPoints_.reserve(constants::MAX_SYNC_POINTS);
    
    // 初始化性能指标
    currentMetrics_ = GPUPerformanceMetrics{};
    previousMetrics_ = GPUPerformanceMetrics{};
    
    isInitialized_ = true;
    return true;
}

void RHIGPUOptimizer::Shutdown() {
    if (!isInitialized_) {
        return;
    }
    
    // 等待所有待提交的命令缓冲区完成
    // device_.WaitIdle();
    
    // 清理资源
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    std::lock_guard<std::mutex> syncLock(syncMutex_);
    
    commandBufferCache_.clear();
    resourceBindingCache_.clear();
    syncPoints_.clear();
    pendingCommandBuffers_.clear();
    
    // 清空队列
    while (!commandBufferQueue_.empty()) {
        commandBufferQueue_.pop();
    }
    
    isInitialized_ = false;
}

void RHIGPUOptimizer::Update(u64 frameNumber, f32 deltaTime) {
    if (!isInitialized_) {
        return;
    }
    
    currentFrameNumber_ = frameNumber;
    accumulatedFrameTime_ += deltaTime;
    performanceUpdateCounter_++;
    
    // 定期更新性能指标
    if (accumulatedFrameTime_ >= constants::PERFORMANCE_SAMPLE_INTERVAL) {
        UpdatePerformanceMetrics(deltaTime);
        accumulatedFrameTime_ = 0.0f;
        performanceUpdateCounter_ = 0;
    }
    
    // 清理过期缓存
    if (frameNumber % 60 == 0) {  // 每秒清理一次
        CleanupExpiredCache();
    }
    
    // 优化同步策略
    if (config_.syncStrategy == SynchronizationStrategy::Adaptive) {
        OptimizeSynchronization();
    }
    
    // 更新资源绑定缓存
    if (config_.enableResourceBindingCache) {
        UpdateResourceBindingCache();
    }
    
    // 自动调整优化策略
    if (performanceUpdateCounter_ % config_.performanceUpdateInterval == 0) {
        AutoAdjustOptimizationStrategy(currentMetrics_);
    }
}

CommandBufferOptimizationInfo RHIGPUOptimizer::OptimizeCommandBuffer(CommandBufferHandle commandBuffer) {
    CommandBufferOptimizationInfo info;
    if (commandBuffer == handles::INVALID_COMMAND_BUFFER) {
        return info;
    }
    
    // 检查缓存
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        auto it = commandBufferCache_.find(commandBuffer);
        if (it != commandBufferCache_.end()) {
            return it->second;
        }
    }
    
    const auto startTime = std::chrono::high_resolution_clock::now();
    
    // 分析命令缓冲区
    info = AnalyzeCommandBuffer(commandBuffer);
    info.handle = commandBuffer;
    
    // 根据优化级别执行不同的优化
    switch (config_.commandLevel) {
        case CommandOptimizationLevel::None:
            // 不进行优化
            break;
            
        case CommandOptimizationLevel::Basic:
            // 基础优化：移除冗余状态切换
            info.redundantStateChanges = RemoveRedundantStateChanges(commandBuffer);
            break;
            
        case CommandOptimizationLevel::Advanced:
            // 高级优化：移除冗余状态切换 + 合并相似命令
            info.redundantStateChanges = RemoveRedundantStateChanges(commandBuffer);
            info.mergedCommands = MergeSimilarCommands(commandBuffer);
            break;
            
        case CommandOptimizationLevel::Maximum:
            // 最大优化：全部优化
            info.redundantStateChanges = RemoveRedundantStateChanges(commandBuffer);
            info.mergedCommands = MergeSimilarCommands(commandBuffer);
            info.optimizedDrawCalls = OptimizeDrawCalls(commandBuffer);
            break;
    }
    
    // 预测执行时间
    info.estimatedExecutionTime = PredictExecutionTime(commandBuffer);
    
    // 计算优化耗时
    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    info.optimizationTime = duration.count() / 1000.0;  // 转换为毫秒
    
    // 缓存结果
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        commandBufferCache_[commandBuffer] = info;
        
        // 限制缓存大小
        if (commandBufferCache_.size() > constants::MAX_CACHED_COMMAND_BUFFERS) {
            // 移除最旧的缓存项
            auto oldestIt = commandBufferCache_.begin();
            for (auto it = commandBufferCache_.begin(); it != commandBufferCache_.end(); ++it) {
                if (it->second.priority < oldestIt->second.priority) {
                    oldestIt = it;
                }
            }
            commandBufferCache_.erase(oldestIt);
        }
    }
    
    // 更新统计信息
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    currentMetrics_.totalCommandsSubmitted += info.commandCount;
    
    return info;
}

bool RHIGPUOptimizer::SubmitOptimizedCommandBuffer(CommandBufferHandle commandBuffer, 
                                                   CommandQueueType queueType,
                                                   u32 priority) {
    // 检查设备有效性
    if (!device_.IsValid()) {
        std::cout << "Debug: SubmitOptimizedCommandBuffer - Device is invalid, rejecting submission" << std::endl;
        return false;
    }
    
    if (commandBuffer == handles::INVALID_COMMAND_BUFFER) {
        return false;
    }
    
    // 优化命令缓冲区
    auto info = OptimizeCommandBuffer(commandBuffer);
    info.priority = priority;
    
    // 根据同步策略处理
    switch (config_.syncStrategy) {
        case SynchronizationStrategy::Immediate:
            // 立即提交
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                pendingCommandBuffers_.push_back(commandBuffer);
            }
            return SubmitCommandBuffer(commandBuffer);
            
        case SynchronizationStrategy::Batched:
            // 批量提交
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                pendingCommandBuffers_.push_back(commandBuffer);
                
                std::cout << "Debug: Batched strategy - pending buffers: " << pendingCommandBuffers_.size() << ", batch size: " << config_.commandBufferBatchSize << std::endl;
                
                if (pendingCommandBuffers_.size() >= config_.commandBufferBatchSize) {
                    std::cout << "Debug: Triggering batch submission" << std::endl;
                    return SubmitCommandBuffersBatchInternal(pendingCommandBuffers_.data(), 
                                                           static_cast<u32>(pendingCommandBuffers_.size()),
                                                           queueType) > 0;
                }
            }
            return true;  // 加入批处理队列
            
        case SynchronizationStrategy::Adaptive:
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                pendingCommandBuffers_.push_back(commandBuffer);
                
                // 自适应策略：根据当前GPU负载和性能指标决定是否提交
                UpdatePerformanceMetrics(static_cast<f32>(currentMetrics_.frameTime));
                
                // 如果GPU利用率高或者帧时间过长，提前提交
                bool shouldSubmit = false;
                if (currentMetrics_.gpuUtilization > config_.gpuUtilizationThreshold) {
                    shouldSubmit = true;
                    std::cout << "Debug: Adaptive strategy - high GPU utilization, submitting early" << std::endl;
                } else if (currentMetrics_.frameTime > config_.targetFrameTime * 1.1f) {
                    shouldSubmit = true;
                    std::cout << "Debug: Adaptive strategy - high frame time, submitting early" << std::endl;
                } else if (pendingCommandBuffers_.size() >= config_.commandBufferBatchSize) {
                    shouldSubmit = true;
                    std::cout << "Debug: Adaptive strategy - batch size reached, submitting" << std::endl;
                }
                
                if (shouldSubmit) {
                    std::cout << "Debug: Adaptive strategy - submitting " << pendingCommandBuffers_.size() << " buffers" << std::endl;
                    return SubmitCommandBuffersBatchInternal(pendingCommandBuffers_.data(), 
                                                           static_cast<u32>(pendingCommandBuffers_.size()),
                                                           queueType) > 0;
                }
            }
            return true;  // 加入待提交队列
            
        case SynchronizationStrategy::Predictive:
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                pendingCommandBuffers_.push_back(commandBuffer);
                
                // 预测策略：基于历史数据预测最优提交时机
                f64 predictedExecTime = PredictExecutionTime(commandBuffer);
                
                bool shouldSubmit = false;
                // 如果预测执行时间较长，立即提交以避免延迟
                if (predictedExecTime > config_.targetFrameTime * 0.5f) {
                    shouldSubmit = true;
                    std::cout << "Debug: Predictive strategy - long execution time predicted, submitting immediately" << std::endl;
                } else if (pendingCommandBuffers_.size() >= config_.commandBufferBatchSize / 2) {
                    // 预测策略使用更小的批处理阈值以提高响应性
                    shouldSubmit = true;
                    std::cout << "Debug: Predictive strategy - predictive batch threshold reached, submitting" << std::endl;
                } else if (currentMetrics_.cpuToGpuLatency > config_.targetFrameTime * 0.3f) {
                    // 如果CPU到GPU延迟较高，提前提交
                    shouldSubmit = true;
                    std::cout << "Debug: Predictive strategy - high CPU-GPU latency, submitting early" << std::endl;
                }
                
                if (shouldSubmit) {
                    std::cout << "Debug: Predictive strategy - submitting " << pendingCommandBuffers_.size() << " buffers" << std::endl;
                    return SubmitCommandBuffersBatchInternal(pendingCommandBuffers_.data(), 
                                                           static_cast<u32>(pendingCommandBuffers_.size()),
                                                           queueType) > 0;
                }
            }
            return true;  // 加入待提交队列
    }
    
    return false;
}

u32 RHIGPUOptimizer::SubmitCommandBuffersBatchInternal(const CommandBufferHandle* commandBuffers, 
                                                        u32 count,
                                                        CommandQueueType queueType) {
    if (!commandBuffers || count == 0) {
        return 0;
    }
    
    u32 submittedCount = 0;
    
    // 检查设备有效性
    if (!device_.IsValid()) {
        std::cout << "Debug: Device is invalid, rejecting batch submission" << std::endl;
        return 0;
    }
    
    // 批量提交命令缓冲区到设备
    std::cout << "Debug: SubmitCommandBuffersBatchInternal - count: " << count << std::endl;
    for (u32 i = 0; i < count; ++i) {
        std::cout << "Debug: Submitting buffer " << i << "/" << count << " with handle " << commandBuffers[i] << std::endl;
        QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = commandBuffers[i];
        bool submitResult = device_.Submit(submitInfo);
        std::cout << "Debug: SubmitCommandBuffer returned " << (submitResult ? "true" : "false") << std::endl;
        if (submitResult) {
            std::cout << "Debug: Adding to queue (no lock needed)..." << std::endl;
            commandBufferQueue_.push(commandBuffers[i]);
            submittedCount++;
            std::cout << "Debug: Buffer " << i << "/" << count << " submitted successfully (" << submittedCount << "/" << count << " total)" << std::endl;
        } else {
            std::cout << "Debug: Buffer " << i << "/" << count << " submission failed" << std::endl;
        }
        std::cout << "Debug: Loop iteration " << i << " completed" << std::endl;
    }
    std::cout << "Debug: Batch submission complete - " << submittedCount << "/" << count << " buffers submitted" << std::endl;
    
    // 创建同步点
    if (submittedCount > 0) {
        CreateSmartSyncPoint(queueType, commandBuffers, submittedCount);
    }
    
    return submittedCount;
}

u32 RHIGPUOptimizer::SubmitCommandBuffersBatch(const CommandBufferHandle* commandBuffers, 
                                               u32 count,
                                               CommandQueueType queueType) {
    if (!commandBuffers || count == 0) {
        return 0;
    }
    
    u32 submittedCount = SubmitCommandBuffersBatchInternal(commandBuffers, count, queueType);
    
    // 更新统计信息
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    currentMetrics_.totalCommandsExecuted += submittedCount;
    
    // 清空待提交列表
    if (submittedCount == count) {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        pendingCommandBuffers_.clear();
    }
    
    return submittedCount;
}

SyncHandle RHIGPUOptimizer::CreateSmartSyncPoint(CommandQueueType queueType,
                                                 const CommandBufferHandle* dependentBuffers,
                                                 u32 dependentCount) {
    SyncPointInfo syncInfo;
    syncInfo.frameNumber = currentFrameNumber_;
    syncInfo.timestamp = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    syncInfo.queueType = queueType;
    syncInfo.dependentCommandBuffers = dependentCount;
    syncInfo.isCompleted = false;
    
    // 创建同步对象（具体实现依赖于平台）
    syncInfo.syncHandle = device_.CreateSync();
    
    {
        std::lock_guard<std::mutex> syncLock(syncMutex_);
        syncPoints_.push_back(syncInfo);
        
        // 限制同步点数量
        if (syncPoints_.size() > constants::MAX_SYNC_POINTS) {
            syncPoints_.erase(syncPoints_.begin());
        }
    }
    
    return syncInfo.syncHandle;
}

bool RHIGPUOptimizer::WaitForSyncPoint(SyncHandle syncHandle, u32 timeoutMs) {
    if (syncHandle == handles::INVALID_SYNC) {
        return false;
    }
    
    const auto startTime = std::chrono::high_resolution_clock::now();
    
    // 等待同步点完成
    bool result = device_.WaitForSync(syncHandle, timeoutMs);
    
    // 更新同步点信息
    {
        std::lock_guard<std::mutex> syncLock(syncMutex_);
        for (auto& syncInfo : syncPoints_) {
            if (syncInfo.syncHandle == syncHandle) {
                syncInfo.isCompleted = result;
                const auto endTime = std::chrono::high_resolution_clock::now();
                const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                syncInfo.waitTime = duration.count();
                break;
            }
        }
    }
    
    return result;
}

bool RHIGPUOptimizer::SubmitCommandBuffer(CommandBufferHandle commandBuffer) {
    // 检查设备有效性
    if (!device_.IsValid()) {
        std::cout << "Debug: Device is invalid, rejecting single command submission" << std::endl;
        return false;
    }
    
    // 提交命令缓冲区到设备
    std::lock_guard<std::mutex> lock(cacheMutex_);
    commandBufferQueue_.push(commandBuffer);
    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = commandBuffer;
    return device_.Submit(submitInfo);
}

void RHIGPUOptimizer::ClearPendingBuffers() {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    pendingCommandBuffers_.clear();
}

f64 RHIGPUOptimizer::GetCurrentTimestamp() {
    // 获取当前时间戳（毫秒）
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double, std::milli>(duration).count();
}

void RHIGPUOptimizer::RecordPassExecutionTime(const std::string& passName, f64 timeMs) {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    currentMetrics_.passExecutionTimes[passName] = timeMs;
}

bool RHIGPUOptimizer::CacheResourceBinding(ResourceHandle resource, u32 bindSlot) {
    if (!config_.enableResourceBindingCache || resource == handles::INVALID_RESOURCE) {
        return false;
    }
    
    const u64 cacheKey = (static_cast<u64>(resource) << 32) | bindSlot;
    
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    
    ResourceBindingCacheItem& cacheItem = resourceBindingCache_[cacheKey];
    cacheItem.resource = resource;
    cacheItem.bindSlot = bindSlot;
    cacheItem.lastUsedFrame = currentFrameNumber_;
    cacheItem.useCount++;
    cacheItem.isDirty = false;
    
    return true;
}

bool RHIGPUOptimizer::IsResourceBindingCached(ResourceHandle resource, u32 bindSlot) const {
    if (!config_.enableResourceBindingCache) {
        return false;
    }
    
    const u64 cacheKey = (static_cast<u64>(resource) << 32) | bindSlot;
    
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    auto it = resourceBindingCache_.find(cacheKey);
    
    return it != resourceBindingCache_.end() && !it->second.isDirty;
}

void RHIGPUOptimizer::InvalidateResourceCache(ResourceHandle resource) {
    if (!config_.enableResourceBindingCache) {
        return;
    }
    
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    
    if (resource == handles::INVALID_RESOURCE) {
        // 清空全部缓存
        for (auto& pair : resourceBindingCache_) {
            pair.second.isDirty = true;
        }
    } else {
        // 清空特定资源缓存
        for (auto& pair : resourceBindingCache_) {
            if (pair.second.resource == resource) {
                pair.second.isDirty = true;
            }
        }
    }
}

u32 RHIGPUOptimizer::OptimizeMemoryAllocation(u64 size, u64 alignment, GPUMemoryUsage usage) {
    // 这里可以集成自适应内存池
    // 简化实现，直接返回0表示需要调用者处理
    return 0;
}

void RHIGPUOptimizer::OptimizeMemoryDeallocation(u32 memoryHandle) {
    // 这里可以集成自适应内存池的释放逻辑
    // 简化实现
}

const char* RHIGPUOptimizer::GetOptimizationStatistics() const {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    std::ostringstream oss;
    oss << "=== RHI GPU优化器统计 ===\n";
    oss << "当前帧编号: " << currentFrameNumber_ << "\n";
    oss << "帧时间: " << std::fixed << std::setprecision(2) << currentMetrics_.frameTime << " ms\n";
    oss << "GPU利用率: " << std::fixed << std::setprecision(1) 
        << (currentMetrics_.gpuUtilization * 100.0) << "%\n";
    oss << "内存带宽利用率: " << std::fixed << std::setprecision(1) 
        << (currentMetrics_.memoryBandwidthUtilization * 100.0) << "%\n";
    oss << "待执行命令缓冲区: " << currentMetrics_.pendingCommandBuffers << "\n";
    oss << "总提交命令数: " << currentMetrics_.totalCommandsSubmitted << "\n";
    oss << "总执行命令数: " << currentMetrics_.totalCommandsExecuted << "\n";
    oss << "平均命令延迟: " << std::fixed << std::setprecision(3) 
        << currentMetrics_.averageCommandLatency << " ms\n";
    oss << "每帧同步点数: " << currentMetrics_.syncPointsPerFrame << "\n";
    oss << "CPU到GPU延迟: " << std::fixed << std::setprecision(3) 
        << currentMetrics_.cpuToGpuLatency << " ms\n";
    oss << "当前内存使用: " << (currentMetrics_.currentMemoryUsage / 1024 / 1024) << " MB\n";
    oss << "峰值内存使用: " << (currentMetrics_.peakMemoryUsage / 1024 / 1024) << " MB\n";
    
    if (!currentMetrics_.passExecutionTimes.empty()) {
        oss << "=== Pass Execution Times ===\n";
        for (const auto& pair : currentMetrics_.passExecutionTimes) {
            oss << pair.first << ": " << std::fixed << std::setprecision(3) << pair.second << " ms\n";
        }
    }

    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        oss << "缓存的命令缓冲区: " << commandBufferCache_.size() << "\n";
        oss << "缓存的资源绑定: " << resourceBindingCache_.size() << "\n";
    }
    
    {
        std::lock_guard<std::mutex> syncLock(syncMutex_);
        oss << "活跃同步点: " << syncPoints_.size() << "\n";
    }
    
    oss << "优化策略: ";
    switch (config_.strategy) {
        case GPUOptimizationStrategy::Conservative: oss << "保守"; break;
        case GPUOptimizationStrategy::Balanced: oss << "平衡"; break;
        case GPUOptimizationStrategy::Aggressive: oss << "激进"; break;
    }
    oss << "\n";
    
    oss << "命令优化级别: ";
    switch (config_.commandLevel) {
        case CommandOptimizationLevel::None: oss << "无"; break;
        case CommandOptimizationLevel::Basic: oss << "基础"; break;
        case CommandOptimizationLevel::Advanced: oss << "高级"; break;
        case CommandOptimizationLevel::Maximum: oss << "最大"; break;
    }
    oss << "\n";
    
    oss << "=========================";
    
    const std::string str = oss.str();
    strncpy(const_cast<char*>(statisticsBuffer_), str.c_str(), sizeof(statisticsBuffer_) - 1);
    statisticsBuffer_[sizeof(statisticsBuffer_) - 1] = '\0';
    
    return statisticsBuffer_;
}

void RHIGPUOptimizer::ResetPerformanceStatistics() {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    currentMetrics_ = GPUPerformanceMetrics{};
    previousMetrics_ = GPUPerformanceMetrics{};
    performanceUpdateCounter_ = 0;
    accumulatedFrameTime_ = 0.0;
}

bool RHIGPUOptimizer::ExportPerformanceReport(const char* filename) const {
    if (!filename) {
        return false;
    }
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    file << GetOptimizationStatistics() << std::endl;
    
    // 添加额外的详细分析
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    file << "\n=== 详细性能分析 ===\n";
    file << "平均帧时间: " << (accumulatedFrameTime_ / std::max(1u, performanceUpdateCounter_)) << " ms\n";
    file << "性能采样间隔: " << constants::PERFORMANCE_SAMPLE_INTERVAL << " 秒\n";
    
    file.close();
    return true;
}

void RHIGPUOptimizer::SetConfig(const OptimizationConfig& config) {
    config_ = config;
    
    // 重新分配容器空间
    if (config.enableResourceBindingCache) {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        resourceBindingCache_.reserve(constants::MAX_RESOURCE_BINDINGS);
    }
    
    if (config.maxConcurrentCommandBuffers > 0) {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        pendingCommandBuffers_.reserve(config.maxConcurrentCommandBuffers);
    }
}

void RHIGPUOptimizer::AutoAdjustOptimizationStrategy(const GPUPerformanceMetrics& metrics) {
    // 根据性能指标自动调整优化策略
    if (metrics.frameTime > config_.targetFrameTime * 1.2f) {
        // 帧时间过高，需要更激进的优化
        config_.strategy = GPUOptimizationStrategy::Aggressive;
        config_.commandLevel = CommandOptimizationLevel::Maximum;
    } else if (metrics.frameTime < config_.targetFrameTime * 0.8f) {
        // 性能充足，可以采用保守策略
        if (config_.strategy != GPUOptimizationStrategy::Conservative) {
            config_.strategy = GPUOptimizationStrategy::Conservative;
            config_.commandLevel = CommandOptimizationLevel::Basic;
        }
    }
    
    // 根据GPU利用率调整同步策略
    if (metrics.gpuUtilization > 0.9f) {
        config_.syncStrategy = SynchronizationStrategy::Adaptive;
    } else if (metrics.gpuUtilization < 0.5f) {
        config_.syncStrategy = SynchronizationStrategy::Immediate;
    }
}

// === 私有方法实现 ===

CommandBufferOptimizationInfo RHIGPUOptimizer::AnalyzeCommandBuffer(CommandBufferHandle commandBuffer) {
    CommandBufferOptimizationInfo info;
    
    // 简化实现：假设有100个命令
    info.commandCount = 100;
    info.optimizedDrawCalls = info.commandCount / 2;  // 假设可以优化一半
    
    return info;
}

u32 RHIGPUOptimizer::RemoveRedundantStateChanges(CommandBufferHandle commandBuffer) {
    // 简化实现：假设移除了10%的冗余状态切换
    return 10;
}

u32 RHIGPUOptimizer::MergeSimilarCommands(CommandBufferHandle commandBuffer) {
    // 简化实现：假设合并了5%的命令
    return 5;
}

u32 RHIGPUOptimizer::OptimizeDrawCalls(CommandBufferHandle commandBuffer) {
    // 简化实现：假设优化了15%的绘制调用
    return 15;
}

f64 RHIGPUOptimizer::PredictExecutionTime(CommandBufferHandle commandBuffer) {
    // 简化实现：基于命令数量预测执行时间
    // 假设每个命令平均耗时0.01毫秒
    return 100 * 0.01;  // 1毫秒
}

void RHIGPUOptimizer::UpdatePerformanceMetrics(f32 deltaTime) {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    // 保存上一帧指标
    previousMetrics_ = currentMetrics_;
    
    // 更新帧时间（平滑处理）
    currentMetrics_.frameTime = currentMetrics_.frameTime * (1.0 - constants::GPU_UTILIZATION_SMOOTHING) + 
                               deltaTime * constants::GPU_UTILIZATION_SMOOTHING;
    
    // 分析GPU利用率
    AnalyzeGPUUtilization();
    
    // 分析内存带宽
    AnalyzeMemoryBandwidth();
    
    // 更新待执行命令缓冲区数量
    {
        std::lock_guard<std::mutex> cacheLock(cacheMutex_);
        currentMetrics_.pendingCommandBuffers = static_cast<u32>(pendingCommandBuffers_.size() + 
                                                                commandBufferQueue_.size());
    }
    
    // 更新同步点统计
    {
        std::lock_guard<std::mutex> syncLock(syncMutex_);
        u32 completedSyncPoints = 0;
        for (const auto& syncInfo : syncPoints_) {
            if (syncInfo.frameNumber == currentFrameNumber_) {
                completedSyncPoints++;
            }
        }
        currentMetrics_.syncPointsPerFrame = completedSyncPoints;
    }
    
    // 更新内存使用统计
    // currentMetrics_.currentMemoryUsage = device_.GetCurrentMemoryUsage(); // 暂时注释掉，接口不存在
    currentMetrics_.peakMemoryUsage = std::max(currentMetrics_.peakMemoryUsage, 
                                              currentMetrics_.currentMemoryUsage);
}

void RHIGPUOptimizer::AnalyzeGPUUtilization() {
    // 简化实现：基于帧时间和目标帧时间估算GPU利用率
    const f64 targetFrameTime = config_.targetFrameTime;
    const f64 utilization = std::min(1.0, currentMetrics_.frameTime / targetFrameTime);
    
    currentMetrics_.gpuUtilization = currentMetrics_.gpuUtilization * (1.0 - constants::GPU_UTILIZATION_SMOOTHING) + 
                                     utilization * constants::GPU_UTILIZATION_SMOOTHING;
}

void RHIGPUOptimizer::AnalyzeMemoryBandwidth() {
    // 简化实现：基于内存分配/释放活动估算带宽利用率
    // 这里使用固定的模拟值
    static f64 simulatedBandwidthUsage = 0.3;  // 30%
    currentMetrics_.memoryBandwidthUtilization = simulatedBandwidthUsage;
    
    // 添加一些随机波动
    simulatedBandwidthUsage += (rand() % 100 - 50) * 0.001;
    simulatedBandwidthUsage = std::max(0.0, std::min(1.0, simulatedBandwidthUsage));
}

void RHIGPUOptimizer::OptimizeSynchronization() {
    // 根据GPU负载动态调整同步策略
    if (currentMetrics_.gpuUtilization > 0.9f) {
        // GPU负载过高，减少同步频率
        config_.syncStrategy = SynchronizationStrategy::Batched;
    } else if (currentMetrics_.gpuUtilization < 0.5f) {
        // GPU负载较低，增加同步频率
        config_.syncStrategy = SynchronizationStrategy::Immediate;
    }
}

void RHIGPUOptimizer::CleanupExpiredCache() {
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    
    // 清理过期的命令缓冲区缓存
    auto it = commandBufferCache_.begin();
    while (it != commandBufferCache_.end()) {
        if (currentFrameNumber_ - it->second.priority > constants::CACHE_EXPIRE_FRAMES) {
            it = commandBufferCache_.erase(it);
        } else {
            ++it;
        }
    }
    
    // 清理过期的资源绑定缓存
    auto rit = resourceBindingCache_.begin();
    while (rit != resourceBindingCache_.end()) {
        if (currentFrameNumber_ - rit->second.lastUsedFrame > constants::CACHE_EXPIRE_FRAMES) {
            rit = resourceBindingCache_.erase(rit);
        } else {
            ++rit;
        }
    }
    
    // 清理已完成的同步点
    std::lock_guard<std::mutex> syncLock(syncMutex_);
    auto sit = syncPoints_.begin();
    while (sit != syncPoints_.end()) {
        if (sit->isCompleted && 
            currentFrameNumber_ - sit->frameNumber > constants::CACHE_EXPIRE_FRAMES) {
            sit = syncPoints_.erase(sit);
        } else {
            ++sit;
        }
    }
}

void RHIGPUOptimizer::UpdateResourceBindingCache() {
    // 更新资源绑定缓存的使用统计
    std::lock_guard<std::mutex> cacheLock(cacheMutex_);
    
    for (auto& pair : resourceBindingCache_) {
        if (pair.second.isDirty) {
            // 标记为过期的缓存项
            pair.second.lastUsedFrame = 0;
        }
    }
}

// === 工厂函数实现 ===

std::unique_ptr<RHIGPUOptimizer> CreateGPUOptimizer(RHIDeviceBase& device, 
                                                    const OptimizationConfig& config) {
    auto optimizer = std::make_unique<RHIGPUOptimizer>(device, config);
    if (optimizer->Initialize()) {
        return optimizer;
    }
    return nullptr;
}

} // namespace primal::graphics::rhi