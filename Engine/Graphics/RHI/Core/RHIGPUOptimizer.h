/**
 * @file RHIGPUOptimizer.h
 * @brief RHI GPU驱动渲染优化器
 * @details 提供智能GPU命令优化、同步管理、资源绑定优化和性能监控功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHIMath.h"
#include "RHITypes.h"
#include "RHIDevice.h"
#include "RHICommand.h"
#include "RHIMpscQueue.h"
#include <queue>

namespace primal::graphics::rhi {

// 类型别名
using f64 = double;

// === 前向声明 ===
class RHIBatchRenderer;
class RHIDeterministicPrefetchManager;

/**
 * @brief GPU优化策略枚举
 */
enum class GPUOptimizationStrategy : u8 {
    Conservative = 0,    ///< 保守策略：优先稳定性
    Balanced = 1,        ///< 平衡策略：性能与稳定性并重
    Aggressive = 2       ///< 激进策略：优先性能
};

/**
 * @brief 命令缓冲区优化级别
 */
enum class CommandOptimizationLevel : u8 {
    None = 0,            ///< 不优化
    Basic = 1,           ///< 基础优化（去除冗余状态切换）
    Advanced = 2,        ///< 高级优化（命令重排序、合并）
    Maximum = 3          ///< 最大优化（预测性预取、智能批处理）
};

/**
 * @brief 同步策略枚举
 */
enum class SynchronizationStrategy : u8 {
    Immediate = 0,       ///< 立即同步：每次提交后等待
    Batched = 1,         ///< 批量同步：累积多个提交后同步
    Adaptive = 2,        ///< 自适应同步：根据GPU负载动态调整
    Predictive = 3       ///< 预测性同步：基于历史数据预测最优同步点
};

/**
 * @brief GPU性能指标
 */
struct GPUPerformanceMetrics {
    f64 frameTime;                    ///< 帧时间（毫秒）
    f64 gpuUtilization;               ///< GPU利用率（0-1）
    f64 memoryBandwidthUtilization;   ///< 内存带宽利用率（0-1）
    f64 commandBufferExecutionTime;   ///< 命令缓冲区执行时间（毫秒）
    u32 pendingCommandBuffers;        ///< 待执行命令缓冲区数量
    u64 totalCommandsSubmitted;       ///< 总提交命令数
    u64 totalCommandsExecuted;        ///< 总执行命令数
    f64 averageCommandLatency;        ///< 平均命令延迟（毫秒）
    u32 syncPointsPerFrame;           ///< 每帧同步点数量
    f64 cpuToGpuLatency;              ///< CPU到GPU延迟（毫秒）
    // Memory metrics
    u64 memoryAllocations;
    u64 memoryDeallocations;
    u64 currentMemoryUsage;
    u64 peakMemoryUsage;
    
    // Per-pass GPU execution time in milliseconds
    std::unordered_map<std::string, double> passExecutionTimes;

    GPUPerformanceMetrics() : frameTime(0.0), gpuUtilization(0.0), 
                              memoryBandwidthUtilization(0.0), commandBufferExecutionTime(0.0),
                              pendingCommandBuffers(0), totalCommandsSubmitted(0), totalCommandsExecuted(0),
                              averageCommandLatency(0.0), syncPointsPerFrame(0), cpuToGpuLatency(0.0),
                              memoryAllocations(0), memoryDeallocations(0), currentMemoryUsage(0), peakMemoryUsage(0) {}
};

/**
 * @brief 优化配置
 */
struct OptimizationConfig {
    GPUOptimizationStrategy strategy;         ///< 优化策略
    CommandOptimizationLevel commandLevel;    ///< 命令优化级别
    SynchronizationStrategy syncStrategy;      ///< 同步策略
    bool enableCommandMerging;                ///< 是否启用命令合并
    bool enableResourceBindingCache;          ///< 是否启用资源绑定缓存
    bool enablePredictivePrefetch;            ///< 是否启用预测性预取
    bool enableAdaptiveBatching;              ///< 是否启用自适应批处理
    bool enableMemoryPooling;                 ///< 是否启用内存池
    u32 maxConcurrentCommandBuffers;           ///< 最大并发命令缓冲区数
    u32 commandBufferBatchSize;                ///< 命令缓冲区批处理大小
    f32 targetFrameTime;                       ///< 目标帧时间（毫秒）
    f32 gpuUtilizationThreshold;               ///< GPU利用率阈值
    u32 performanceUpdateInterval;             ///< 性能更新间隔（帧）
    
    OptimizationConfig() : strategy(GPUOptimizationStrategy::Balanced),
                          commandLevel(CommandOptimizationLevel::Advanced),
                          syncStrategy(SynchronizationStrategy::Adaptive),
                          enableCommandMerging(true), enableResourceBindingCache(true),
                          enablePredictivePrefetch(true), enableAdaptiveBatching(true),
                          enableMemoryPooling(true), maxConcurrentCommandBuffers(8),
                          commandBufferBatchSize(4), targetFrameTime(16.67f),
                          gpuUtilizationThreshold(0.85f), performanceUpdateInterval(60) {}
};

/**
 * @brief 命令缓冲区优化信息
 */
struct CommandBufferOptimizationInfo {
    CommandBufferHandle handle;                ///< 命令缓冲区句柄
    u32 commandCount;                          ///< 命令数量
    u32 redundantStateChanges;                 ///< 冗余状态切换数量
    u32 mergedCommands;                        ///< 合并的命令数量
    u32 optimizedDrawCalls;                    ///< 优化后的绘制调用数
    f64 optimizationTime;                      ///< 优化耗时（毫秒）
    f64 estimatedExecutionTime;                ///< 预估执行时间（毫秒）
    u32 priority;                               ///< 优先级
    bool isCritical;                            ///< 是否为关键路径
    
    CommandBufferOptimizationInfo() : handle(handles::INVALID_COMMAND_BUFFER), commandCount(0),
                                    redundantStateChanges(0), mergedCommands(0),
                                    optimizedDrawCalls(0), optimizationTime(0.0),
                                    estimatedExecutionTime(0.0), priority(0), isCritical(false) {}
};

/**
 * @brief 资源绑定缓存项
 */
struct ResourceBindingCacheItem {
    ResourceHandle resource;                   ///< 资源句柄
    u32 bindSlot;                              ///< 绑定槽位
    u64 lastUsedFrame;                         ///< 最后使用帧
    u32 useCount;                              ///< 使用次数
    bool isDirty;                              ///< 是否需要更新
    
    ResourceBindingCacheItem() : resource(handles::INVALID_RESOURCE), bindSlot(0),
                                lastUsedFrame(0), useCount(0), isDirty(true) {}
};

/**
 * @brief 同步点信息
 */
struct SyncPointInfo {
    SyncHandle syncHandle;                     ///< 同步句柄
    u64 frameNumber;                           ///< 帧编号
    f64 timestamp;                             ///< 时间戳
    CommandQueueType queueType;                ///< 队列类型
    u32 dependentCommandBuffers;               ///< 依赖的命令缓冲区数量
    bool isCompleted;                          ///< 是否已完成
    f64 waitTime;                              ///< 等待时间
    
    SyncPointInfo() : syncHandle(handles::INVALID_SYNC), frameNumber(0), timestamp(0.0),
                     queueType(CommandQueueType::Unknown), dependentCommandBuffers(0),
                     isCompleted(false), waitTime(0.0) {}
};

/**
 * @brief RHI GPU驱动渲染优化器
 * @details 提供智能的GPU命令优化、同步管理和性能监控功能
 */
class RHIGPUOptimizer {
public:
    /**
     * @brief 构造函数
     * @param device RHI设备引用
     * @param config 优化配置
     */
    RHIGPUOptimizer(RHIDeviceBase& device, const OptimizationConfig& config = OptimizationConfig{});
    
    /**
     * @brief 析构函数
     */
    ~RHIGPUOptimizer();
    
    // === 核心功能接口 ===
    
    /**
     * @brief 初始化GPU优化器
     * @return 是否成功
     */
    bool Initialize();
    
    /**
     * @brief 关闭GPU优化器
     */
    void Shutdown();
    
    /**
     * @brief 每帧更新
     * @param frameNumber 当前帧编号
     * @param deltaTime 帧时间
     */
    void Update(u64 frameNumber, f32 deltaTime);
    
    /**
     * @brief 优化命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @return 优化信息
     */
    CommandBufferOptimizationInfo OptimizeCommandBuffer(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 提交优化的命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @param queueType 队列类型
     * @param priority 优先级
     * @return 提交是否成功
     */
    bool SubmitOptimizedCommandBuffer(CommandBufferHandle commandBuffer, 
                                     CommandQueueType queueType = CommandQueueType::Graphics,
                                     u32 priority = 0);
    
    /**
     * @brief 批量提交命令缓冲区
     * @param commandBuffers 命令缓冲区数组
     * @param count 数量
     * @param queueType 队列类型
     * @return 提交的命令缓冲区数量
     */
    u32 SubmitCommandBuffersBatch(const CommandBufferHandle* commandBuffers, u32 count,
                                  CommandQueueType queueType = CommandQueueType::Graphics);

    // === 同步点管理 ===
    
    /**
     * @brief 导出性能报告
     * @param filename 文件名
     * @return 是否成功
     */
    bool ExportPerformanceReport(const char* filename) const;

    // === 同步点管理 ===
    
    /**
     * @brief 创建智能同步点
     * @param queueType 队列类型
     * @param dependentBuffers 依赖的命令缓冲区
     * @param dependentCount 依赖数量
     * @return 同步点句柄
     */
    SyncHandle CreateSmartSyncPoint(CommandQueueType queueType,
                                   const CommandBufferHandle* dependentBuffers = nullptr,
                                   u32 dependentCount = 0);
    
    /**
     * @brief 等待同步点
     * @param syncPoint 同步点句柄
     * @param timeoutMs 超时时间（毫秒）
     * @return 是否成功
     */
    bool WaitForSyncPoint(SyncHandle syncPoint, u32 timeoutMs = UINT32_MAX);
    
    /**
     * @brief 提交命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @return 是否成功
     */
    bool SubmitCommandBuffer(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 获取当前时间戳
     * @return 时间戳（毫秒）
     */
    f64 GetCurrentTimestamp();

    /**
     * @brief 记录Pass执行时间
     * @param passName Pass名称
     * @param timeMs 执行时间（毫秒）
     */
    void RecordPassExecutionTime(const std::string& passName, f64 timeMs);
    
    // === 资源管理优化 ===
    
    /**
     * @brief 缓存资源绑定
     * @param resource 资源句柄
     * @param bindSlot 绑定槽位
     * @return 是否成功缓存
     */
    bool CacheResourceBinding(ResourceHandle resource, u32 bindSlot);
    
    /**
     * @brief 检查资源绑定是否已缓存
     * @param resource 资源句柄
     * @param bindSlot 绑定槽位
     * @return 是否已缓存
     */
    bool IsResourceBindingCached(ResourceHandle resource, u32 bindSlot) const;
    
    /**
     * @brief 使缓存失效
     * @param resource 资源句柄（可选，INVALID_RESOURCE表示全部）
     */
    void InvalidateResourceCache(ResourceHandle resource = handles::INVALID_RESOURCE);
    
    /**
     * @brief 优化内存分配
     * @param size 分配大小
     * @param alignment 对齐要求
     * @param usage 内存用途
     * @return 优化后的内存句柄
     */
    u32 OptimizeMemoryAllocation(u64 size, u64 alignment, GPUMemoryUsage usage);
    
    /**
     * @brief 释放优化内存
     * @param memoryHandle 内存句柄
     */
    void OptimizeMemoryDeallocation(u32 memoryHandle);
    
    // === 性能监控和分析 ===
    
    /**
     * @brief 获取当前性能指标
     * @return 性能指标
     */
    const GPUPerformanceMetrics& GetPerformanceMetrics() const { return currentMetrics_; }
    
    /**
     * @brief 获取优化统计信息
     * @return 统计信息字符串
     */
    const char* GetOptimizationStatistics() const;
    
    /**
     * @brief 重置性能统计
     */
    void ResetPerformanceStatistics();
    
    /**
     * @brief 设置性能回调
     * @param callback 回调函数
     */
    void SetPerformanceCallback(std::function<void(const GPUPerformanceMetrics&)> callback);
    
    /**
     * @brief 清除待提交的命令缓冲区（主要用于测试）
     */
    void ClearPendingBuffers();
    
    // === 配置管理 ===
    
    /**
     * @brief 获取优化配置
     * @return 配置引用
     */
    const OptimizationConfig& GetConfig() const { return config_; }
    
    /**
     * @brief 设置优化配置
     * @param config 新配置
     */
    void SetConfig(const OptimizationConfig& config);
    
    /**
     * @brief 自动调整优化策略
     * @param metrics 当前性能指标
     */
    void AutoAdjustOptimizationStrategy(const GPUPerformanceMetrics& metrics);

private:
    /**
     * @brief 批量提交命令缓冲区（内部版本，假设已持有锁）
     * @param commandBuffers 命令缓冲区数组
     * @param count 缓冲区数量
     * @param queueType 队列类型
     * @return 成功提交的缓冲区数量
     */
    u32 SubmitCommandBuffersBatchInternal(const CommandBufferHandle* commandBuffers, u32 count,
                                           CommandQueueType queueType);
    
public:
    // === 内部优化方法（测试需要访问） ===
    
    /**
     * @brief 分析命令缓冲区
     * @param commandBuffer 命令缓冲区
     * @return 分析结果
     */
    CommandBufferOptimizationInfo AnalyzeCommandBuffer(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 移除冗余状态切换
     * @param commandBuffer 命令缓冲区
     * @return 移除的冗余切换数量
     */
    u32 RemoveRedundantStateChanges(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 合并相似命令
     * @param commandBuffer 命令缓冲区
     * @return 合并的命令数量
     */
    u32 MergeSimilarCommands(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 优化绘制调用
     * @param commandBuffer 命令缓冲区
     * @return 优化后的绘制调用数
     */
    u32 OptimizeDrawCalls(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 预测执行时间
     * @param commandBuffer 命令缓冲区
     * @return 预估执行时间
     */
    f64 PredictExecutionTime(CommandBufferHandle commandBuffer);
    
    /**
     * @brief 更新性能指标
     * @param deltaTime 帧时间
     */
    void UpdatePerformanceMetrics(f32 deltaTime);
    
    /**
     * @brief 分析GPU利用率
     */
    void AnalyzeGPUUtilization();
    
    /**
     * @brief 分析内存带宽
     */
    void AnalyzeMemoryBandwidth();
    
    /**
     * @brief 优化同步策略
     */
    void OptimizeSynchronization();
    
    /**
     * @brief 清理过期的缓存项
     */
    void CleanupExpiredCache();
    
    /**
     * @brief 更新资源绑定缓存
     */
    void UpdateResourceBindingCache();
    
private:
    
    // === 成员变量 ===
    
    RHIDeviceBase& device_;                       ///< RHI设备引用
    OptimizationConfig config_;                    ///< 优化配置
    
    GPUPerformanceMetrics currentMetrics_;        ///< 当前性能指标
    GPUPerformanceMetrics previousMetrics_;       ///< 上一帧性能指标
    
    std::unordered_map<CommandBufferHandle, CommandBufferOptimizationInfo> commandBufferCache_;  ///< 命令缓冲区缓存
    std::unordered_map<u64, ResourceBindingCacheItem> resourceBindingCache_;                     ///< 资源绑定缓存
    utl::vector<SyncPointInfo> syncPoints_;        ///< 同步点列表
    
    utl::vector<CommandBufferHandle> pendingCommandBuffers_;  ///< 待提交命令缓冲区
    std::queue<CommandBufferHandle> commandBufferQueue_;      ///< 命令缓冲区队列
    
    u64 currentFrameNumber_;                       ///< 当前帧编号
    u32 performanceUpdateCounter_;                 ///< 性能更新计数器
    f64 accumulatedFrameTime_;                     ///< 累积帧时间
    
    mutable char statisticsBuffer_[4096];          ///< 统计信息缓冲区
    bool isInitialized_;                           ///< 是否已初始化
    
    // 线程安全
    mutable std::mutex cacheMutex_;               ///< 缓存互斥锁
    mutable std::mutex metricsMutex_;              ///< 性能指标互斥锁
    mutable std::mutex syncMutex_;                ///< 同步互斥锁
};

/**
 * @brief 创建GPU优化器
 * @param device RHI设备引用
 * @param config 优化配置
 * @return GPU优化器指针
 */
std::unique_ptr<RHIGPUOptimizer> CreateGPUOptimizer(RHIDeviceBase& device, 
                                                    const OptimizationConfig& config = OptimizationConfig{});

} // namespace primal::graphics::rhi