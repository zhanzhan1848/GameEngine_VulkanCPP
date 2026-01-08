/**
 * @file RHIMultiThreadedCommandGenerator.cpp
 * @brief RHI多线程命令生成器实现
 * @details 提供高性能的并行命令缓冲区生成功能，支持多线程渲染命令构建
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include <chrono>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iostream>

#include "RHIMultiThreadedCommandGenerator.h"
#include "RHIDevice.h"
#include "RHIMemoryPool.h"
#include "RHIMpscQueue.h"

namespace primal::graphics::rhi {

// Helper function to get current time in milliseconds
u64 GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// === WorkerThreadPool 实现 ===

/**
 * @brief 工作线程本地上下文
 * @details 每个工作线程都有自己的本地上下文，减少线程间竞争
 */
struct ThreadLocalContext {
    std::unique_ptr<RHIMemoryPool> memoryPool;      ///< 线程本地内存池
    std::vector<CommandGenerationTask> taskQueue;  ///< 线程本地任务队列
    std::vector<CommandGenerationResult> results;  ///< 线程本地结果队列
    u32 processedTaskCount;                         ///< 已处理任务数量
    f64 totalProcessingTime;                       ///< 总处理时间
    u64 lastCleanupTime;                           ///< 上次清理时间
    
    ThreadLocalContext() : processedTaskCount(0), totalProcessingTime(0.0), lastCleanupTime(0) {
        // RHIMemoryPool是抽象类，不能直接实例化
        // memoryPool = std::make_unique<RHIMemoryPool>(1024 * 1024); // 1MB初始大小
        taskQueue.reserve(64);
        results.reserve(64);
    }
};

/**
 * @brief 工作线程池实现
 * @details 管理多个工作线程，提供任务提交和同步功能
 */
class WorkerThreadPool {
public:
    /**
     * @brief 构造函数
     * @param threadCount 线程数量
     */
    explicit WorkerThreadPool(RHIDeviceBase& device, u32 threadCount) 
        : threadCount_(threadCount), activeTasks_(0), shutdownFlag_(false), device_(device) {
        workerThreads_.reserve(threadCount_);
    }
    
    /**
     * @brief 析构函数
     */
    ~WorkerThreadPool() {
        Shutdown();
    }
    
    /**
     * @brief 初始化线程池
     * @return 是否初始化成功
     */
    bool Initialize() {
        shutdownFlag_ = false;
        
        // 创建队列配置
        QueueConfig queueConfig;
        queueConfig.maxQueueSize = 10000;
        queueConfig.maxWorkItemSize = sizeof(CommandGenerationTask);
        queueConfig.enableBatching = true;
        queueConfig.batchSize = 32;
        
        // 创建任务队列
        taskQueue_ = std::make_unique<DefaultMpscQueue>(device_, queueConfig);
        if (!taskQueue_->Initialize()) {
            std::cerr << "Failed to initialize task queue" << std::endl;
            return false;
        }
        
        if (!taskQueue_->Start()) {
            std::cerr << "Failed to start task queue" << std::endl;
            return false;
        }
        
        for (u32 i = 0; i < threadCount_; ++i) {
            workerThreads_.emplace_back(&WorkerThreadPool::WorkerThreadFunction, this, i);
        }
        return true;
    }

    /**
     * @brief 关闭线程池
     */
    void Shutdown() {
        if (!shutdownFlag_.exchange(true)) {
            if (taskQueue_) {
                taskQueue_->WakeUpAllConsumers();
            }
            for (auto& thread : workerThreads_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            workerThreads_.clear();
        }
    }
    
    /**
     * @brief 提交任务到队列
     * @param task 任务
     * @return 是否提交成功
     */
    bool SubmitTask(CommandGenerationTask task) {
        if (!shutdownFlag_) {
            activeTasks_++;
            
            // 将CommandGenerationTask转换为WorkItem
            WorkItem workItem;
            workItem.type = WorkItemType::CustomCallback;
            workItem.priority = WorkPriority::Normal;
            workItem.state = WorkItemState::Pending;
            workItem.id = 0; // 将由队列分配
            workItem.timestamp = GetCurrentTimeMs();
            workItem.timeoutMs = 5000;
            workItem.context = new CommandGenerationTask(std::move(task));
            
            // 创建回调函数来执行实际的命令生成
            CommandGenerationTask* taskPtr = static_cast<CommandGenerationTask*>(workItem.context);
            workItem.callbackData.callback = new std::function<void()>([this, taskPtr]() {
                if (taskPtr) {
                    GenerateCommandsForTask(*taskPtr);
                    delete taskPtr;
                }
                activeTasks_--;
            });
            
            if (taskQueue_->Enqueue(workItem) == 0) {
                // Enqueue failed, decrement active tasks and clean up
                activeTasks_--;
                delete taskPtr;
                return false;
            }
            return true;
        }
        return false;
    }
    
    /**
     * @brief 获取并清除结果
     * @return 结果列表
     */
    std::vector<CommandGenerationResult> GetAndClearResults() {
        std::lock_guard<std::mutex> lock(resultMutex_);
        std::vector<CommandGenerationResult> results = std::move(completedResults_);
        completedResults_.clear();
        return results;
    }

    /**
     * @brief 为单个任务生成命令
     * @param task 命令生成任务
     */
    void GenerateCommandsForTask(const CommandGenerationTask& task) {
        CommandGenerationResult result;
        result.taskId = task.taskId;
        // 获取当前线程ID（简单模拟）
        result.threadId = 0; 
        result.success = true;
        
        auto startTime = std::chrono::high_resolution_clock::now();

        if (task.taskFunction) {
             result.commandBuffer = task.taskFunction(task.renderBatch);
             if (result.commandBuffer == handles::INVALID_COMMAND_BUFFER) {
                 result.success = false;
             } else {
                 result.generatedCommands = task.renderBatch.commandCount;
             }
        } else {
            // 模拟
             result.commandBuffer = handles::INVALID_COMMAND_BUFFER;
             result.generatedCommands = 0;
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.generationTimeMs = std::chrono::duration<f64, std::milli>(endTime - startTime).count();
        
        StoreResult(result);
    }
    
    /**
     * @brief 等待所有任务完成
     */
    void WaitForAllTasks() {
        while (activeTasks_.load() > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    /**
     * @brief 设置屏障
     * @details 等待当前所有任务完成后再接受新任务
     */
    void SetBarrier() {
        WaitForAllTasks();
    }
    
    /**
     * @brief 获取活跃线程数量
     * @return 活跃线程数量
     */
    u32 GetActiveThreadCount() const {
        return threadCount_;
    }
    
    /**
     * @brief 获取活跃任务数量
     * @return 活跃任务数量
     */
    u32 GetActiveTaskCount() const {
        return activeTasks_.load();
    }

private:
    /**
     * @brief 工作线程函数
     * @param threadId 线程ID
     */
    void WorkerThreadFunction(u32 threadId) {
        thread_local ThreadLocalContext threadContext;
        
        while (!shutdownFlag_.load()) {
            WorkItem workItem;
            if (taskQueue_->TryDequeue(workItem)) {
                auto startTime = std::chrono::high_resolution_clock::now();
                
                // 从WorkItem中提取CommandGenerationTask并执行
                    if (workItem.type == WorkItemType::CustomCallback && workItem.callbackData.callback) {
                        (*workItem.callbackData.callback)();
                    }
                
                auto endTime = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
                
                // 更新统计信息
                threadContext.processedTaskCount++;
                threadContext.totalProcessingTime += duration.count() / 1000.0;
                
                // StoreResult函数需要根据实际情况调整或注释掉
                // StoreResult(result);
                
                // activeTasks_在回调函数中处理，这里不再递减
                
                // 定期清理本地资源
                auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
                if (currentTime - threadContext.lastCleanupTime > 5000) { // 5秒清理一次
                    threadContext.taskQueue.clear();
                    threadContext.results.clear();
                    threadContext.lastCleanupTime = currentTime;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
    
    /**
     * @brief 处理单个任务
     * @param task 任务
     * @param threadId 线程ID
     * @param context 线程本地上下文
     * @return 处理结果
     */
    CommandGenerationResult ProcessTask(const CommandGenerationTask& task, u32 threadId, ThreadLocalContext& context) {
        CommandGenerationResult result;
        result.taskId = task.taskId;
        result.threadId = threadId;
        result.success = true;
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        if (task.taskFunction) {
            // 使用自定义回调生成命令
            result.commandBuffer = task.taskFunction(task.renderBatch);
            
            // 如果返回了无效句柄，视为失败
            if (result.commandBuffer == handles::INVALID_COMMAND_BUFFER) {
                result.success = false;
            } else {
                // 假设生成了一些命令，这里简单设置为批次命令数
                // 实际应该从 CommandBuffer 获取
                result.generatedCommands = task.renderBatch.commandCount;
            }
        } else {
            // 默认模拟逻辑
            result.generatedCommands = task.renderBatch.commandCount;
            result.commandBuffer = reinterpret_cast<CommandBufferHandle>(0x100000000ULL | task.taskId);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        result.generationTimeMs = std::chrono::duration<f64, std::milli>(endTime - startTime).count();
        
        return result;
    }
    
    /**
     * @brief 存储任务结果
     * @param result 结果
     */
    void StoreResult(const CommandGenerationResult& result) {
        // 在实际实现中应该使用线程安全的结果收集器
        // 这里暂时使用简单的方式
        std::lock_guard<std::mutex> lock(resultMutex_);
        completedResults_.push_back(result);
    }
    
    u32 threadCount_;                                    ///< 线程数量
    std::vector<std::thread> workerThreads_;            ///< 工作线程列表
    std::unique_ptr<RHIMpscQueue> taskQueue_;           ///< 任务队列
    std::atomic<u32> activeTasks_;                       ///< 活跃任务数量
    std::atomic<bool> shutdownFlag_;                     ///< 关闭标志
    RHIDeviceBase& device_;                              ///< 设备引用
    
    // 结果收集
    std::vector<CommandGenerationResult> completedResults_; ///< 已完成任务结果
    std::mutex resultMutex_;                             ///< 结果互斥锁
    
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(WorkerThreadPool);
};

// === TaskDistributor 实现 ===

/**
 * @brief 任务分发器实现
 * @details 负责将渲染场景分割成适合多线程处理的任务
 */
class TaskDistributor {
public:
    /**
     * @brief 构造函数
     */
    TaskDistributor() = default;
    
    /**
     * @brief 析构函数
     */
    ~TaskDistributor() = default;
    
    /**
     * @brief 分发工作任务
     * @param scene 渲染场景
     * @param threadCount 线程数量
     * @return 生成的任务列表
     */
    std::vector<CommandGenerationTask> DistributeWorkItems(const RenderScene& scene, u32 threadCount) {
        std::vector<CommandGenerationTask> tasks;
        
        if (scene.totalDrawCalls == 0) {
            return tasks;
        }
        
        // 计算每个线程应该处理的批次大小
        u32 batchesPerThread = std::max<u32>(1u, static_cast<u32>((scene.totalDrawCalls + threadCount - 1) / threadCount));
        u32 totalBatches = (scene.totalDrawCalls + batchesPerThread - 1) / batchesPerThread;
        
        tasks.reserve(totalBatches);
        
        // 创建任务
        for (u32 batchId = 0; batchId < totalBatches; ++batchId) {
            CommandGenerationTask task;
            task.taskId = batchId;
            task.priority = CalculateTaskPriority(scene, batchId, totalBatches);
            
            // 计算批次范围
            u32 startDrawCall = batchId * batchesPerThread;
            u32 endDrawCall = std::min(startDrawCall + batchesPerThread, static_cast<u32>(scene.totalDrawCalls));
            
            // 填充渲染批次数据
            MultiThreadRenderBatch& batch = task.renderBatch;
            batch.batchId = batchId;
            batch.startDrawCallIndex = startDrawCall;
            batch.endDrawCallIndex = endDrawCall;
            batch.renderTarget = scene.renderTarget;
            
            // 计算网格索引范围（简化实现）
            batch.startMeshIndex = startDrawCall;
            batch.endMeshIndex = std::min(endDrawCall, static_cast<u32>(scene.meshes.size()));
            
            // 估算命令数量（简化实现，实际应该基于渲染批次内容）
            batch.commandCount = (endDrawCall - startDrawCall) * 10; // 假设每个绘制调用平均10个命令
            
            // 复制批次数据
            if (batch.endMeshIndex > batch.startMeshIndex) {
                batch.batchMeshes.assign(scene.meshes.begin() + batch.startMeshIndex,
                                        scene.meshes.begin() + batch.endMeshIndex);
                batch.batchTransforms.assign(scene.transforms.begin() + batch.startMeshIndex,
                                            scene.transforms.begin() + batch.endMeshIndex);
                batch.batchMaterialIndices.assign(scene.materialIndices.begin() + batch.startMeshIndex,
                                                 scene.materialIndices.begin() + batch.endMeshIndex);
            }
            
            tasks.push_back(std::move(task));
        }
        
        return tasks;
    }

private:
    /**
     * @brief 计算任务优先级
     * @param scene 渲染场景
     * @param batchId 批次ID
     * @param totalBatches 总批次数
     * @return 优先级
     */
    u32 CalculateTaskPriority(const RenderScene& scene, u32 batchId, u32 totalBatches) {
        // 简单的优先级计算：前面的批次优先级更高
        // 实际实现中可以基于距离、重要性等因素计算
        f32 normalizedPosition = static_cast<f32>(batchId) / static_cast<f32>(totalBatches);
        return static_cast<u32>((1.0f - normalizedPosition) * 3); // 0-3优先级
    }
    
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(TaskDistributor);
};

// === ResultMerger 实现 ===

/**
 * @brief 结果合并器实现
 * @details 负责合并多个线程生成的命令缓冲区
 */
class ResultMerger {
public:
    /**
     * @brief 构造函数
     */
    ResultMerger() = default;
    
    /**
     * @brief 析构函数
     */
    ~ResultMerger() = default;
    
    /**
     * @brief 合并任务结果
     * @param results 任务结果列表
     * @param outputs 合并后的输出命令缓冲区列表
     * @return 是否合并成功
     */
    bool MergeTaskResults(const std::vector<CommandGenerationResult>& results,
                          std::vector<CommandBufferHandle>& outputs) {
        outputs.clear();
        outputs.reserve(results.size());
        
        // 按任务ID排序结果，确保执行顺序正确
        std::vector<CommandGenerationResult> sortedResults = results;
        std::sort(sortedResults.begin(), sortedResults.end(),
                 [](const CommandGenerationResult& a, const CommandGenerationResult& b) {
                     return a.taskId < b.taskId;
                 });
        
        // 验证所有任务都成功完成
        for (const auto& result : sortedResults) {
            if (!result.success) {
                std::cerr << "Task " << result.taskId << " failed on thread " << result.threadId << std::endl;
                return false;
            }
        }
        
        // 合并命令缓冲区
        for (const auto& result : sortedResults) {
            if (result.commandBuffer != handles::INVALID_COMMAND_BUFFER) {
                outputs.push_back(result.commandBuffer);
            }
        }
        
        return !outputs.empty();
    }
    
    /**
     * @brief 获取合并统计信息
     * @param results 结果列表
     * @param totalTime 总时间
     * @param outputCommands 输出命令数量
     * @param outputBuffers 输出缓冲区数量
     */
    void GetMergeStats(const std::vector<CommandGenerationResult>& results,
                      f64& totalTime, u64& outputCommands, u64& outputBuffers) {
        totalTime = 0.0;
        outputCommands = 0;
        outputBuffers = 0;
        
        for (const auto& result : results) {
            totalTime += result.generationTimeMs;
            outputCommands += result.generatedCommands;
            if (result.success && result.commandBuffer != handles::INVALID_COMMAND_BUFFER) {
                outputBuffers++;
            }
        }
    }

private:
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(ResultMerger);
};

// === MultiThreadPerformanceAnalyzer 实现 ===

/**
 * @brief 多线程性能分析器实现
 * @details 收集和分析多线程命令生成的性能数据
 */
class MultiThreadPerformanceAnalyzer {
public:
    /**
     * @brief 构造函数
     */
    MultiThreadPerformanceAnalyzer() : frameCount_(0), lastUpdateTime_(0) {
        frameHistory_.reserve(1000);
    }
    
    /**
     * @brief 析构函数
     */
    ~MultiThreadPerformanceAnalyzer() = default;
    
    /**
     * @brief 更新性能指标
     * @param taskResults 任务结果
     * @param threadCount 线程数量
     * @param activeTasks 活跃任务数量
     */
    void UpdateMetrics(const std::vector<CommandGenerationResult>& taskResults,
                      u32 threadCount, u32 activeTasks) {
        auto currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        // 计算基础指标
        CalculateBasicMetrics(taskResults, threadCount, activeTasks);
        
        // 更新历史数据
        frameCount_++;
        if (currentTime - lastUpdateTime_ > 1000) { // 每秒更新一次
            CalculateAdvancedMetrics();
            lastUpdateTime_ = currentTime;
        }
    }
    
    /**
     * @brief 获取当前性能指标
     * @return 性能指标
     */
    const MultiThreadPerformanceMetrics& GetMetrics() const {
        return metrics_;
    }
    
    /**
     * @brief 重置性能统计
     */
    void ResetMetrics() {
        metrics_ = MultiThreadPerformanceMetrics{};
        frameCount_ = 0;
        frameHistory_.clear();
    }
    
    /**
     * @brief 导出性能报告
     * @param filename 文件名
     * @return 是否导出成功
     */
    bool ExportReport(const char* filename) const {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        
        file << "=== Multi-Threaded Command Generator Performance Report ===\n\n";
        file << "Frame Count: " << frameCount_ << "\n\n";
        
        file << "=== Throughput Metrics ===\n";
        file << "Commands Generated Per Second: " << metrics_.commandsGeneratedPerSecond << "\n";
        file << "Command Buffers Generated Per Second: " << metrics_.commandBuffersGeneratedPerSecond << "\n";
        file << "Average Batch Processing Time: " << metrics_.averageBatchProcessingTimeMs << " ms\n\n";
        
        file << "=== Latency Metrics ===\n";
        file << "Average Generation Time: " << metrics_.averageGenerationTimeMs << " ms\n";
        file << "Max Generation Time: " << metrics_.maxGenerationTimeMs << " ms\n";
        file << "Min Generation Time: " << metrics_.minGenerationTimeMs << " ms\n";
        file << "95th Percentile Generation Time: " << metrics_.p95GenerationTimeMs << " ms\n\n";
        
        file << "=== Utilization Metrics ===\n";
        file << "CPU Utilization: " << metrics_.cpuUtilizationPercent << "%\n";
        file << "Thread Utilization: " << metrics_.threadUtilizationPercent << "%\n";
        file << "Active Worker Threads: " << metrics_.activeWorkerThreads << "\n";
        file << "Idle Worker Threads: " << metrics_.idleWorkerThreads << "\n\n";
        
        file.close();
        return true;
    }

private:
    /**
     * @brief 计算基础性能指标
     */
    void CalculateBasicMetrics(const std::vector<CommandGenerationResult>& results,
                              u32 threadCount, u32 activeTasks) {
        if (results.empty()) {
            return;
        }
        
        u64 totalCommands = 0;
        f64 totalTime = 0.0;
        f64 maxTime = 0.0;
        f64 minTime = std::numeric_limits<f64>::max();
        
        for (const auto& result : results) {
            if (result.success) {
                totalCommands += result.generatedCommands;
                totalTime += result.generationTimeMs;
                maxTime = std::max(maxTime, result.generationTimeMs);
                minTime = std::min(minTime, result.generationTimeMs);
            }
        }
        
        metrics_.averageGenerationTimeMs = totalTime / results.size();
        metrics_.maxGenerationTimeMs = maxTime;
        metrics_.minGenerationTimeMs = minTime;
        metrics_.activeWorkerThreads = activeTasks;
        metrics_.idleWorkerThreads = threadCount - activeTasks;
        
        if (threadCount > 0) {
            metrics_.threadUtilizationPercent = (static_cast<f64>(activeTasks) / threadCount) * 100.0;
        }
    }
    
    /**
     * @brief 计算高级性能指标
     */
    void CalculateAdvancedMetrics() {
        // 计算每秒生成的命令数
        if (frameCount_ > 0) {
            // 这里需要累积历史数据来计算吞吐量
            // 简化实现
            metrics_.commandsGeneratedPerSecond = 10000.0; // 示例值
            metrics_.commandBuffersGeneratedPerSecond = 60.0; // 示例值
        }
        
        // 计算CPU利用率（简化实现）
        metrics_.cpuUtilizationPercent = metrics_.threadUtilizationPercent;
    }
    
    MultiThreadPerformanceMetrics metrics_;  ///< 性能指标
    u64 frameCount_;                         ///< 帧计数
    u64 lastUpdateTime_;                     ///< 上次更新时间
    std::vector<f64> frameHistory_;          ///< 帧历史数据
    
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(MultiThreadPerformanceAnalyzer);
};

// === RHIMultiThreadedCommandGenerator 主类实现 ===

RHIMultiThreadedCommandGenerator::RHIMultiThreadedCommandGenerator(RHIDeviceBase& device, 
                                                                   const MultiThreadConfig& config)
    : device_(device), config_(config), isInitialized_(false), isShuttingDown_(false),
      activeTaskCount_(0), systemStatus_("Uninitialized") {
    
    // 初始化子系统
    workerPool_ = std::make_unique<WorkerThreadPool>(device, config.workerThreadCount);
    taskDistributor_ = std::make_unique<TaskDistributor>();
    resultMerger_ = std::make_unique<ResultMerger>();
    performanceAnalyzer_ = std::make_unique<MultiThreadPerformanceAnalyzer>();
    
    // 预分配容器空间
    pendingTasks_.reserve(MAX_PENDING_TASKS);
    completedResults_.reserve(MAX_COMPLETED_RESULTS);
}

RHIMultiThreadedCommandGenerator::~RHIMultiThreadedCommandGenerator() {
    Shutdown();
}

bool RHIMultiThreadedCommandGenerator::Initialize() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    if (isInitialized_) {
        return true;
    }
    
    if (config_.workerThreadCount == 0) {
        systemStatus_ = "Invalid configuration: 0 worker threads";
        return false;
    }
    
    // 初始化工作线程池
    if (!workerPool_->Initialize()) {
        systemStatus_ = "Failed to initialize worker thread pool";
        return false;
    }
    
    isShuttingDown_ = false;
    isInitialized_ = true;
    systemStatus_ = "Initialized";
    
    return true;
}

void RHIMultiThreadedCommandGenerator::Shutdown() {
    if (isShuttingDown_.exchange(true)) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    // 等待所有任务完成
    WaitForAllTasks();
    
    // 关闭工作线程池
    workerPool_->Shutdown();
    
    // 清理资源
    CleanupExpiredResources();
    
    isInitialized_ = false;
    systemStatus_ = "Shutdown";
}

bool RHIMultiThreadedCommandGenerator::GenerateCommandsParallel(const RenderScene& scene, 
                                                                std::vector<CommandBufferHandle>& outputs) {
    if (!isInitialized_) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        systemStatus_ = "Not initialized";
        return false;
    }
    
    if (scene.totalDrawCalls == 0) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            systemStatus_ = "Empty scene";
        }
        outputs.clear();
        return true;
    }
    
    {
        // 获取线程数配置（使用锁保护）
        u32 workerThreadCount = GetWorkerThreadCount();
        
        // 分发工作任务
        auto tasks = taskDistributor_->DistributeWorkItems(scene, workerThreadCount);
        
        if (tasks.empty()) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            systemStatus_ = "No tasks generated";
            return false;
        }
        
        // 提交任务到线程池
    for (auto& task : tasks) {
        // 设置回调函数
        task.taskFunction = customGenerationFunc_;
        
        // 使用简单的自旋重试机制处理队列满的情况
        // 在实际引擎中，这里可能需要更复杂的背压控制或丢帧策略
        while (!workerPool_->SubmitTask(task)) {
            // 队列已满，让出时间片等待消费者处理
            std::this_thread::yield();
        }
    }
        
        // 等待任务完成
        workerPool_->WaitForAllTasks();
        
        // 收集任务结果
        std::vector<CommandGenerationResult> results = workerPool_->GetAndClearResults();
        
        // 验证结果数量
        if (results.size() != tasks.size()) {
             // 处理结果丢失的情况
             std::cerr << "Warning: Expected " << tasks.size() << " results, got " << results.size() << std::endl;
        }
        
        // 合并结果
        bool success = resultMerger_->MergeTaskResults(results, outputs);
        
        // 更新性能指标
        bool enablePerf = false;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            enablePerf = config_.enablePerformanceMonitoring;
        }
        
        if (enablePerf) {
            performanceAnalyzer_->UpdateMetrics(results, workerThreadCount, 
                                             static_cast<u32>(tasks.size()));
            UpdatePerformanceMetrics();
        }
        
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            systemStatus_ = success ? "Success" : "Merge failed";
        }
        return success;
    }
}

bool RHIMultiThreadedCommandGenerator::GenerateCommandBuffer(const RenderScene& scene, 
                                                           CommandBufferHandle& commandBuffer) {
    // 简化实现：重用并行逻辑，但只用一个线程
    // 实际生产代码可能会有专门的单线程优化路径
    std::vector<CommandBufferHandle> outputs;
    bool result = GenerateCommandsParallel(scene, outputs);
    
    if (result && !outputs.empty()) {
        commandBuffer = outputs[0];
        return true;
    }
    
    return false;
}

void RHIMultiThreadedCommandGenerator::SetCommandGenerationCallback(std::function<CommandBufferHandle(const MultiThreadRenderBatch&)> callback) {
    customGenerationFunc_ = std::move(callback);
}

void RHIMultiThreadedCommandGenerator::SetWorkerThreadCount(u32 count) {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (config_.workerThreadCount != count) {
        // 需要重新初始化线程池
        workerPool_->Shutdown();
        
        config_.workerThreadCount = count;
        workerPool_ = std::make_unique<WorkerThreadPool>(device_, count);
        workerPool_->Initialize();
    }
}

u32 RHIMultiThreadedCommandGenerator::GetWorkerThreadCount() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_.workerThreadCount;
}

u32 RHIMultiThreadedCommandGenerator::GetActiveThreadCount() const {
    return workerPool_->GetActiveThreadCount();
}

bool RHIMultiThreadedCommandGenerator::WaitForAllTasks(u32 timeoutMs) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    while (workerPool_->GetActiveTaskCount() > 0) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        
        if (elapsed.count() >= timeoutMs) {
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    return true;
}

void RHIMultiThreadedCommandGenerator::UpdateConfig(const MultiThreadConfig& config) {
    // 先检查是否需要改变线程数（不持有锁）
    auto currentThreadCount = workerPool_->GetActiveThreadCount();
    bool needChangeThreadCount = (currentThreadCount != config.workerThreadCount);
    
    // 更新配置
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
    }
    
    // 如果需要改变线程数，重新初始化线程池
    if (needChangeThreadCount) {
        SetWorkerThreadCount(config.workerThreadCount);
    }
}

const MultiThreadConfig& RHIMultiThreadedCommandGenerator::GetConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

void RHIMultiThreadedCommandGenerator::SetPerformanceMonitoringEnabled(bool enable) {
    std::lock_guard<std::mutex> lock(configMutex_);
    config_.enablePerformanceMonitoring = enable;
}

const MultiThreadPerformanceMetrics& RHIMultiThreadedCommandGenerator::GetPerformanceMetrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    return performanceAnalyzer_->GetMetrics();
}

void RHIMultiThreadedCommandGenerator::ResetPerformanceMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    performanceAnalyzer_->ResetMetrics();
}

bool RHIMultiThreadedCommandGenerator::ExportPerformanceReport(const char* filename) const {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    return performanceAnalyzer_->ExportReport(filename);
}

u32 RHIMultiThreadedCommandGenerator::GetDebugInfo(char* buffer, u32 bufferSize) const {
    std::ostringstream oss;
    oss << "=== RHIMultiThreadedCommandGenerator Debug Info ===\n";
    oss << "Status: " << systemStatus_ << "\n";
    oss << "Initialized: " << (isInitialized_ ? "Yes" : "No") << "\n";
    oss << "Worker Threads: " << GetWorkerThreadCount() << "\n";
    oss << "Active Threads: " << GetActiveThreadCount() << "\n";
    oss << "Performance Monitoring: " << (config_.enablePerformanceMonitoring ? "Enabled" : "Disabled") << "\n";
    
    std::string info = oss.str();
    u32 copyLength = std::min(bufferSize - 1, static_cast<u32>(info.length()));
    std::memcpy(buffer, info.c_str(), copyLength);
    buffer[copyLength] = '\0';
    
    return copyLength;
}

bool RHIMultiThreadedCommandGenerator::ValidateSystemState() const {
    if (!isInitialized_) {
        return false;
    }
    
    if (config_.workerThreadCount == 0) {
        return false;
    }
    
    if (!workerPool_) {
        return false;
    }
    
    return true;
}

const char* RHIMultiThreadedCommandGenerator::GetSystemStatus() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return systemStatus_.c_str();
}

// === 私有方法实现 ===

void RHIMultiThreadedCommandGenerator::UpdatePerformanceMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    // 性能指标已经在performanceAnalyzer_中更新
    currentMetrics_ = performanceAnalyzer_->GetMetrics();
}

void RHIMultiThreadedCommandGenerator::HandleTaskCompletion(const CommandGenerationResult& result) {
    // 处理任务完成回调
    if (result.success) {
        // 更新统计信息
        activeTaskCount_--;
        
        // 清理过期资源
        static u32 frameCounter = 0;
        if (++frameCounter >= CLEANUP_INTERVAL_FRAMES) {
            CleanupExpiredResources();
            frameCounter = 0;
        }
    }
}

void RHIMultiThreadedCommandGenerator::CleanupExpiredResources() {
    // 清理命令缓冲区缓存
    auto it = commandBufferCache_.begin();
    while (it != commandBufferCache_.end()) {
        // 在实际实现中应该检查引用计数和使用时间
        // 这里简化实现，清理一半的缓存
        if (commandBufferCache_.size() > config_.commandCacheSize / 2) {
            it = commandBufferCache_.erase(it);
        } else {
            ++it;
        }
    }
    
    // 清理资源使用缓存
    auto resIt = resourceUsageCache_.begin();
    while (resIt != resourceUsageCache_.end()) {
        // 简化实现
        if (resourceUsageCache_.size() > config_.resourceCacheSize / 2) {
            resIt = resourceUsageCache_.erase(resIt);
        } else {
            ++resIt;
        }
    }
}

} // namespace primal::graphics::rhi