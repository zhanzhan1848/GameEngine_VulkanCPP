/**
 * @file RHIMultiThreadedCommandGenerator.h
 * @brief RHI多线程命令生成器
 * @details 提供高性能的并行命令缓冲区生成功能，支持多线程渲染命令构建
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"
#include "RHIDevice.h"
#include "RHICommand.h"
#include "RHIMpscQueue.h"
#include "RHIBatchRenderer.h"

namespace primal::graphics::rhi {

// === 前向声明 ===
class WorkerThreadPool;
class TaskDistributor;
class ResultMerger;
class MultiThreadPerformanceAnalyzer;
struct RenderScene;
struct RenderBatch;
struct CommandGenerationResult;
class CommandBuffer;

// === 类型别名 ===
using f32 = float;
using f64 = double;

/**
 * @brief 渲染场景数据结构
 * @details 包含需要渲染的所有对象和资源信息
 */
struct RenderScene {
    std::vector<ResourceHandle> meshes;           ///< 网格资源列表
    std::vector<ResourceHandle> materials;        ///< 材质资源列表
    std::vector<ResourceHandle> textures;         ///< 纹理资源列表
    std::vector<math::m4x4> transforms;        ///< 变换矩阵列表
    std::vector<u32> materialIndices;             ///< 材质索引列表
    std::vector<u32> meshIndices;                 ///< 网格索引列表
    std::vector<u32> drawCalls;                   ///< 绘制调用参数
    u64 totalDrawCalls;                           ///< 总绘制调用数
    u64 totalVertices;                           ///< 总顶点数
    u64 totalTriangles;                          ///< 总三角形数
    f32 boundingSphereRadius;                     ///< 包围球半径
    math::v3 boundingBoxMin;                ///< 包围盒最小值
    math::v3 boundingBoxMax;                ///< 包围盒最大值
    ResourceHandle renderTarget{handles::INVALID_RESOURCE}; ///< 渲染目标

    /**
     * @brief 构造函数
     */
    RenderScene() : totalDrawCalls(0), totalVertices(0), totalTriangles(0),
                   boundingSphereRadius(0.0f),
                   boundingBoxMin(0.0f), boundingBoxMax(0.0f) {}
    
    /**
     * @brief 预分配空间
     * @param meshCount 网格数量
     * @param drawCallCount 绘制调用数量
     */
    void Reserve(u32 meshCount, u32 drawCallCount) {
        meshes.reserve(meshCount);
        materials.reserve(meshCount);
        textures.reserve(meshCount * 4); // 假设每个网格平均4个纹理
        transforms.reserve(meshCount);
        materialIndices.reserve(meshCount);
        meshIndices.reserve(meshCount);
        drawCalls.reserve(drawCallCount);
    }
};

/**
 * @brief 多线程渲染批次数据
 * @details 从渲染场景中分割出的一部分数据，用于单个线程处理
 */
struct MultiThreadRenderBatch {
    u32 batchId;                                 ///< 批次ID
    u32 startMeshIndex;                          ///< 起始网格索引
    u32 endMeshIndex;                            ///< 结束网格索引
    u32 startDrawCallIndex;                      ///< 起始绘制调用索引
    u32 endDrawCallIndex;                        ///< 结束绘制调用索引
    std::vector<ResourceHandle> batchMeshes;     ///< 批次网格列表
    std::vector<math::m4x4> batchTransforms; ///< 批次变换列表
    std::vector<u32> batchMaterialIndices;       ///< 批次材质索引
    u64 commandCount;                           ///< 预估命令数量
    ResourceHandle renderTarget{handles::INVALID_RESOURCE}; ///< 渲染目标
    
    /**
     * @brief 构造函数
     */
    MultiThreadRenderBatch() : batchId(0), startMeshIndex(0), endMeshIndex(0),
                   startDrawCallIndex(0), endDrawCallIndex(0), commandCount(0) {}
};

/**
 * @brief 命令生成任务结构
 * @details 包含单个线程需要执行的命令生成任务信息
 */
struct CommandGenerationTask {
    u32 taskId;                                  ///< 任务ID
    u32 threadId;                                ///< 执行线程ID
    u32 priority;                                ///< 任务优先级
    MultiThreadRenderBatch renderBatch;         ///< 渲染批次数据
    std::function<void(const CommandGenerationResult&)> callback; ///< 完成回调
    std::function<CommandBufferHandle(const MultiThreadRenderBatch&)> taskFunction; ///< 实际命令生成函数
    
    /**
     * @brief 构造函数
     */
    CommandGenerationTask() : taskId(0), threadId(0), priority(0) {}
};

/**
 * @brief 命令生成结果
 * @details 单个任务执行完成后的结果
 */
struct CommandGenerationResult {
    u32 taskId;                                  ///< 任务ID
    u32 threadId;                                ///< 执行线程ID
    CommandBufferHandle commandBuffer;          ///< 生成的命令缓冲区
    u32 generatedCommands;                       ///< 生成的命令数量
    f64 generationTimeMs;                       ///< 生成耗时（毫秒）
    bool success;                                ///< 是否成功
    
    /**
     * @brief 构造函数
     */
    CommandGenerationResult() : taskId(0), threadId(0), 
                             commandBuffer(handles::INVALID_COMMAND_BUFFER),
                             generatedCommands(0), generationTimeMs(0.0f), success(false) {}
};

/**
 * @brief 多线程配置结构
 * @details 配置多线程命令生成器的行为参数
 */
struct MultiThreadConfig {
    u32 workerThreadCount;                       ///< 工作线程数量
    bool enablePerformanceMonitoring;            ///< 是否启用性能监控
    bool enableAdaptiveTuning;                   ///< 是否启用自适应调优
    bool enableMemoryOptimization;               ///< 是否启用内存优化
    bool enableCacheOptimization;                ///< 是否启用缓存优化
    u32 maxConcurrentTasks;                      ///< 最大并发任务数
    u32 commandCacheSize;                        ///< 命令缓存大小
    u32 resourceCacheSize;                       ///< 资源缓存大小
    u32 performanceReportIntervalFrames;        ///< 性能报告间隔帧数
    bool autoExportPerformanceReports;          ///< 是否自动导出性能报告
    
    /**
     * @brief 构造函数
     */
    MultiThreadConfig() : workerThreadCount(std::thread::hardware_concurrency()),
                        enablePerformanceMonitoring(true),
                        enableAdaptiveTuning(true),
                        enableMemoryOptimization(true),
                        enableCacheOptimization(true),
                        maxConcurrentTasks(16),
                        commandCacheSize(1024),
                        resourceCacheSize(512),
                        performanceReportIntervalFrames(60),
                        autoExportPerformanceReports(false) {}
};

/**
 * @brief 多线程性能指标
 * @details 收集和报告多线程命令生成器的性能数据
 */
struct MultiThreadPerformanceMetrics {
    // 吞吐量指标
    f64 commandsGeneratedPerSecond;             ///< 每秒生成命令数
    f64 commandBuffersGeneratedPerSecond;       ///< 每秒生成命令缓冲区数
    f64 averageBatchProcessingTimeMs;          ///< 平均批次处理时间
    
    // 延迟指标
    f64 averageGenerationTimeMs;                ///< 平均生成时间
    f64 maxGenerationTimeMs;                    ///< 最大生成时间
    f64 minGenerationTimeMs;                    ///< 最小生成时间
    f64 p95GenerationTimeMs;                    ///< 95百分位生成时间
    
    // 利用率指标
    f64 cpuUtilizationPercent;                  ///< CPU利用率百分比
    f64 threadUtilizationPercent;               ///< 线程利用率百分比
    u32 activeWorkerThreads;                    ///< 活跃工作线程数
    u32 idleWorkerThreads;                      ///< 空闲工作线程数
    
    // 内存指标
    u64 totalMemoryUsageBytes;                  ///< 总内存使用量（字节）
    u64 peakMemoryUsageBytes;                   ///< 峰值内存使用量（字节）
    f64 memoryFragmentationPercent;             ///< 内存碎片百分比
    
    // 同步指标
    u64 contextSwitchesPerSecond;               ///< 每秒上下文切换次数
    f64 averageWaitTimeMs;                      ///< 平均等待时间
    u64 lockContentions;                        ///< 锁竞争次数
    
    // 缓存指标
    f64 commandCacheHitRate;                    ///< 命令缓存命中率
    f64 resourceCacheHitRate;                   ///< 资源缓存命中率
    u64 cacheEvictionsPerSecond;                ///< 每秒缓存驱逐次数
    
    /**
     * @brief 构造函数
     */
    MultiThreadPerformanceMetrics() : commandsGeneratedPerSecond(0.0),
                                    commandBuffersGeneratedPerSecond(0.0),
                                    averageBatchProcessingTimeMs(0.0),
                                    averageGenerationTimeMs(0.0),
                                    maxGenerationTimeMs(0.0),
                                    minGenerationTimeMs(0.0),
                                    p95GenerationTimeMs(0.0),
                                    cpuUtilizationPercent(0.0),
                                    threadUtilizationPercent(0.0),
                                    activeWorkerThreads(0),
                                    idleWorkerThreads(0),
                                    totalMemoryUsageBytes(0),
                                    peakMemoryUsageBytes(0),
                                    memoryFragmentationPercent(0.0),
                                    contextSwitchesPerSecond(0),
                                    averageWaitTimeMs(0.0),
                                    lockContentions(0),
                                    commandCacheHitRate(0.0),
                                    resourceCacheHitRate(0.0),
                                    cacheEvictionsPerSecond(0) {}
};

/**
 * @brief RHI多线程命令生成器主类
 * @details 提供高性能的并行命令缓冲区生成功能，支持动态线程管理和性能优化
 */
class RHIMultiThreadedCommandGenerator {
public:
    /**
     * @brief 构造函数
     * @param device RHI设备引用
     * @param config 多线程配置
     */
    explicit RHIMultiThreadedCommandGenerator(RHIDeviceBase& device, 
                                             const MultiThreadConfig& config = MultiThreadConfig{});
    
    /**
     * @brief 析构函数
     */
    ~RHIMultiThreadedCommandGenerator();
    
    // === 核心功能接口 ===
    
    /**
     * @brief 初始化多线程命令生成器
     * @return 是否初始化成功
     */
    bool Initialize();
    
    /**
     * @brief 关闭多线程命令生成器
     */
    void Shutdown();
    
    /**
     * @brief 并行生成命令缓冲区
     * @param scene 渲染场景数据
     * @param outputs 输出的命令缓冲区列表
     * @return 是否生成成功
     */
    bool GenerateCommandsParallel(const RenderScene& scene, 
                                 std::vector<CommandBufferHandle>& outputs);
    
    /**
     * @brief 生成单个命令缓冲区（同步版本）
     * @param scene 渲染场景数据
     * @param commandBuffer 输出的命令缓冲区
     * @return 是否生成成功
     */
    bool GenerateCommandBuffer(const RenderScene& scene, 
                              CommandBufferHandle& commandBuffer);
                              
    /**
     * @brief 设置自定义命令生成回调
     * @param callback 回调函数
     */
    void SetCommandGenerationCallback(std::function<CommandBufferHandle(const MultiThreadRenderBatch&)> callback);
    
    // === 线程管理接口 ===
    
    /**
     * @brief 设置工作线程数量
     * @param count 线程数量
     */
    void SetWorkerThreadCount(u32 count);
    
    /**
     * @brief 获取工作线程数量
     * @return 当前工作线程数量
     */
    u32 GetWorkerThreadCount() const;
    
    /**
     * @brief 获取活跃线程数量
     * @return 当前活跃线程数量
     */
    u32 GetActiveThreadCount() const;
    
    /**
     * @brief 等待所有任务完成
     * @param timeoutMs 超时时间（毫秒）
     * @return 是否所有任务都完成
     */
    bool WaitForAllTasks(u32 timeoutMs = UINT32_MAX);
    
    // === 配置管理接口 ===
    
    /**
     * @brief 更新配置
     * @param config 新的配置
     */
    void UpdateConfig(const MultiThreadConfig& config);
    
    /**
     * @brief 获取当前配置
     * @return 当前配置
     */
    const MultiThreadConfig& GetConfig() const;
    
    /**
     * @brief 启用/禁用性能监控
     * @param enable 是否启用
     */
    void SetPerformanceMonitoringEnabled(bool enable);
    
    // === 性能监控接口 ===
    
    /**
     * @brief 获取当前性能指标
     * @return 性能指标
     */
    const MultiThreadPerformanceMetrics& GetPerformanceMetrics() const;
    
    /**
     * @brief 重置性能统计
     */
    void ResetPerformanceMetrics();
    
    /**
     * @brief 导出性能报告
     * @param filename 文件名
     * @return 是否导出成功
     */
    bool ExportPerformanceReport(const char* filename) const;
    
    // === 调试和诊断接口 ===
    
    /**
     * @brief 获取调试信息
     * @param buffer 输出缓冲区
     * @param bufferSize 缓冲区大小
     * @return 实际写入的字符数
     */
    u32 GetDebugInfo(char* buffer, u32 bufferSize) const;
    
    /**
     * @brief 验证系统状态
     * @return 验证结果
     */
    bool ValidateSystemState() const;
    
    /**
     * @brief 获取系统状态描述
     * @return 状态描述字符串
     */
    const char* GetSystemStatus() const;
    
private:
    std::function<CommandBufferHandle(const MultiThreadRenderBatch&)> customGenerationFunc_;

    // === 内部实现方法 ===
    
    /**
     * @brief 分发工作任务
     * @param scene 渲染场景
     * @return 生成的任务列表
     */
    std::vector<CommandGenerationTask> DistributeWorkItems(const RenderScene& scene);
    
    /**
     * @brief 等待任务完成
     * @param tasks 任务列表
     * @return 是否所有任务都成功完成
     */
    bool WaitForTaskCompletion(const std::vector<CommandGenerationTask>& tasks);
    
    /**
     * @brief 合并任务结果
     * @param results 任务结果列表
     * @param outputs 合并后的输出命令缓冲区列表
     * @return 是否合并成功
     */
    bool MergeTaskResults(const std::vector<CommandGenerationResult>& results,
                          std::vector<CommandBufferHandle>& outputs);
    
    /**
     * @brief 更新性能指标
     */
    void UpdatePerformanceMetrics();
    
    /**
     * @brief 处理任务完成回调
     * @param result 任务结果
     */
    void HandleTaskCompletion(const CommandGenerationResult& result);
    
    /**
     * @brief 清理过期资源
     */
    void CleanupExpiredResources();
    
    // === 成员变量 ===
    
    RHIDeviceBase& device_;                       ///< RHI设备引用
    MultiThreadConfig config_;                    ///< 多线程配置
    
    std::unique_ptr<WorkerThreadPool> workerPool_; ///< 工作线程池
    std::unique_ptr<TaskDistributor> taskDistributor_; ///< 任务分发器
    std::unique_ptr<ResultMerger> resultMerger_; ///< 结果合并器
    std::unique_ptr<MultiThreadPerformanceAnalyzer> performanceAnalyzer_; ///< 性能分析器
    
    std::atomic<bool> isInitialized_;            ///< 是否已初始化
    std::atomic<bool> isShuttingDown_;           ///< 是否正在关闭
    
    mutable std::mutex configMutex_;            ///< 配置互斥锁
    mutable std::mutex metricsMutex_;            ///< 性能指标互斥锁
    mutable std::mutex stateMutex_;               ///< 状态互斥锁
    
    MultiThreadPerformanceMetrics currentMetrics_; ///< 当前性能指标
    std::string systemStatus_;                    ///< 系统状态描述
    
    // 任务管理
    std::atomic<u32> activeTaskCount_;           ///< 活跃任务数量
    std::vector<CommandGenerationTask> pendingTasks_; ///< 待处理任务列表
    std::vector<CommandGenerationResult> completedResults_; ///< 已完成任务结果列表
    
    // 资源管理
    std::unordered_map<u32, CommandBufferHandle> commandBufferCache_; ///< 命令缓冲区缓存
    std::unordered_map<ResourceHandle, u64> resourceUsageCache_; ///< 资源使用缓存
    
    // 常量定义
    static constexpr u32 MAX_PENDING_TASKS = 1024; ///< 最大待处理任务数
    static constexpr u32 MAX_COMPLETED_RESULTS = 512; ///< 最大已完成结果数
    static constexpr u32 CLEANUP_INTERVAL_FRAMES = 300; ///< 清理间隔帧数
    static constexpr f64 PERFORMANCE_UPDATE_INTERVAL_MS = 1000.0; ///< 性能更新间隔（毫秒）
    
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(RHIMultiThreadedCommandGenerator);
};

} // namespace primal::graphics::rhi