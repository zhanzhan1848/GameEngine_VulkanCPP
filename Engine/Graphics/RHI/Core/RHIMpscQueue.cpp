/**
 * @file RHIMpscQueue.cpp
 * @brief RHI MPSC工作队列实现
 * @details 基于moodycamel::ConcurrentQueue实现高性能多生产者单消费者工作队列
 * 
 * @author RHI开发团队
 * @date 2025-12-29
 * @version 2.0
 */

#include "RHIMpscQueue.h"
#include "RHIDevice.h"
#include "RHICommand.h"
#include "RHIMemoryPool.h"
#include "../../../../../Engine/Utilities/Logger.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <condition_variable>

namespace primal::graphics::rhi {

// === DefaultMpscQueue 实现 ===

DefaultMpscQueue::DefaultMpscQueue(RHIDevice& device, const QueueConfig& config)
    : RHIMpscQueue(device, config)
    , workQueue_()
    , workerThread_()
    , shutdownRequested_(false)
    , initialized_(false) {
    
    // 预分配优先级队列
    for (uint32_t i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
}

DefaultMpscQueue::~DefaultMpscQueue() {
    Stop();
}

bool DefaultMpscQueue::Initialize() {
    if (initialized_) {
        Logger::Warn("DefaultMpscQueue::Initialize - 队列已经初始化");
        return true;
    }

    try {
        // 预分配工作队列空间
        workQueue_ = moodycamel::ConcurrentQueue<WorkItem>(config_.maxQueueSize);
        
        // 重置统计信息
        stats_.totalEnqueued.store(0);
        stats_.totalDequeued.store(0);
        stats_.totalProcessed.store(0);
        stats_.totalCompleted.store(0);
        stats_.totalFailed.store(0);
        stats_.totalCancelled.store(0);
        stats_.currentQueueSize.store(0);
        stats_.maxQueueSize.store(0);
        stats_.totalProcessingTime.store(0);
        
        // 清空工作项状态映射
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            workItemStates_.clear();
        }
        
        initialized_ = true;
        
        Logger::Info("DefaultMpscQueue::Initialize - MPSC队列初始化成功，最大队列大小: {}", config_.maxQueueSize);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("DefaultMpscQueue::Initialize - 初始化失败: {}", e.what());
        return false;
    }
}

bool DefaultMpscQueue::Start() {
    if (!initialized_) {
        Logger::Error("DefaultMpscQueue::Start - 队列未初始化");
        return false;
    }

    if (running_.load()) {
        Logger::Warn("DefaultMpscQueue::Start - 队列已在运行");
        return true;
    }

    try {
        shutdownRequested_.store(false);
        
        // 启动工作线程
        workerThread_ = std::thread(&DefaultMpscQueue::workerThreadFunc, this);
        
        running_.store(true);
        
        Logger::Info("DefaultMpscQueue::Start - MPSC队列启动成功");
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("DefaultMpscQueue::Start - 启动失败: {}", e.what());
        return false;
    }
}

void DefaultMpscQueue::stopImpl() {
    shutdownRequested_.store(true);
    
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    
    Logger::Info("DefaultMpscQueue::stopImpl - MPSC队列已停止");
}

void DefaultMpscQueue::destroyImpl() {
    // 清空所有队列
    workQueue_ = moodycamel::ConcurrentQueue<WorkItem>();
    
    for (uint32_t i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
    
    // 清空工作项状态映射
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        workItemStates_.clear();
        workConditions_.clear();
    }
    
    initialized_.store(false);
    
    Logger::Info("DefaultMpscQueue::destroyImpl - MPSC队列已销毁");
}

uint64_t DefaultMpscQueue::Enqueue(const WorkItem& workItem) {
    if (!initialized_ || !running_.load()) {
        Logger::Error("DefaultMpscQueue::Enqueue - 队列未初始化或未运行");
        return 0;
    }

    // 检查队列大小限制
    uint32_t currentSize = GetCurrentSize();
    if (currentSize >= config_.maxQueueSize) {
        Logger::Warn("DefaultMpscQueue::Enqueue - 队列已满，无法提交工作项");
        stats_.totalFailed.fetch_add(1);
        return 0;
    }

    return enqueueByPriority(workItem);
}

uint32_t DefaultMpscQueue::EnqueueBatch(const WorkItem* workItems, uint32_t count) {
    if (!initialized_ || !running_.load() || !workItems) {
        Logger::Error("DefaultMpscQueue::EnqueueBatch - 参数无效或队列未就绪");
        return 0;
    }

    uint32_t successCount = 0;
    uint32_t currentSize = GetCurrentSize();
    
    for (uint32_t i = 0; i < count && currentSize < config_.maxQueueSize; ++i) {
        if (enqueueByPriority(workItems[i]) != 0) {
            successCount++;
            currentSize++;
        }
    }
    
    Logger::Debug("DefaultMpscQueue::EnqueueBatch - 批量提交完成，成功: {}/{}", successCount, count);
    return successCount;
}

bool DefaultMpscQueue::Dequeue(WorkItem& workItem) {
    if (!initialized_) {
        return false;
    }

    return dequeueByPriority(workItem);
}

uint32_t DefaultMpscQueue::DequeueBatch(WorkItem* workItems, uint32_t maxCount) {
    if (!initialized_ || !workItems || maxCount == 0) {
        return 0;
    }

    uint32_t dequeuedCount = 0;
    
    for (uint32_t i = 0; i < maxCount; ++i) {
        if (!dequeueByPriority(workItems[i])) {
            break;
        }
        dequeuedCount++;
    }
    
    return dequeuedCount;
}

bool DefaultMpscQueue::CancelWork(uint64_t workId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    auto it = workItemStates_.find(workId);
    if (it != workItemStates_.end() && it->second == WorkItemState::Pending) {
        it->second = WorkItemState::Cancelled;
        stats_.totalCancelled.fetch_add(1);
        
        Logger::Debug("DefaultMpscQueue::CancelWork - 工作项已取消: {}", workId);
        return true;
    }
    
    return false;
}

WorkItemState DefaultMpscQueue::GetWorkState(uint64_t workId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    auto it = workItemStates_.find(workId);
    if (it != workItemStates_.end()) {
        return it->second;
    }
    
    return WorkItemState::Unknown;
}

bool DefaultMpscQueue::WaitForWork(uint64_t workId, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    
    auto it = workItemStates_.find(workId);
    if (it == workItemStates_.end()) {
        return false;
    }
    
    // 创建条件变量（如果不存在）
    std::condition_variable* cond = nullptr;
    auto condIt = workConditions_.find(workId);
    if (condIt == workConditions_.end()) {
        cond = new std::condition_variable();
        workConditions_[workId] = cond;
    } else {
        cond = condIt->second;
    }
    
    // 等待工作完成或超时
    if (timeoutMs == 0) {
        cond->wait(lock, [&] { 
            return it->second == WorkItemState::Completed || 
                   it->second == WorkItemState::Failed || 
                   it->second == WorkItemState::Cancelled; 
        });
    } else {
        auto timeout = std::chrono::milliseconds(timeoutMs);
        bool result = cond->wait_for(lock, timeout, [&] { 
            return it->second == WorkItemState::Completed || 
                   it->second == WorkItemState::Failed || 
                   it->second == WorkItemState::Cancelled; 
        });
        
        if (!result) {
            Logger::Warn("DefaultMpscQueue::WaitForWork - 等待超时: {}", workId);
            return false;
        }
    }
    
    WorkItemState finalState = it->second;
    
    // 清理条件变量
    if (condIt != workConditions_.end()) {
        delete cond;
        workConditions_.erase(condIt);
    }
    
    return finalState == WorkItemState::Completed;
}

bool DefaultMpscQueue::Validate() const {
    if (!initialized_) {
        return false;
    }
    
    // 验证统计信息的一致性
    uint64_t enqueued = stats_.totalEnqueued.load();
    uint64_t dequeued = stats_.totalDequeued.load();
    uint64_t processed = stats_.totalProcessed.load();
    uint64_t completed = stats_.totalCompleted.load();
    uint64_t failed = stats_.totalFailed.load();
    uint64_t cancelled = stats_.totalCancelled.load();
    
    // 处理的数量应该等于完成+失败+取消的数量
    if (processed != (completed + failed + cancelled)) {
        Logger::Error("DefaultMpscQueue::Validate - 统计信息不一致: processed={}, completed+failed+cancelled={}", 
                     processed, completed + failed + cancelled);
        return false;
    }
    
    return true;
}

bool DefaultMpscQueue::TryDequeue(WorkItem& workItem) {
    return Dequeue(workItem);
}

void DefaultMpscQueue::WaitForIdle() {
    while (!IsEmpty() || GetCurrentSize() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DefaultMpscQueue::Clear() {
    // 清空所有队列
    workQueue_ = moodycamel::ConcurrentQueue<WorkItem>();
    
    for (uint32_t i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
    
    // 更新统计信息
    uint32_t currentSize = stats_.currentQueueSize.load();
    stats_.totalDequeued.fetch_add(currentSize);
    stats_.currentQueueSize.store(0);
    
    Logger::Info("DefaultMpscQueue::Clear - 清空队列，清除 {} 个工作项", currentSize);
}

bool DefaultMpscQueue::IsEmpty() const {
    // 检查所有队列是否为空
    if (workQueue_.size_approx() > 0) {
        return false;
    }
    
    for (uint32_t i = 0; i < PRIORITY_LEVELS; ++i) {
        if (priorityQueues_[i].size_approx() > 0) {
            return false;
        }
    }
    
    return true;
}

uint32_t DefaultMpscQueue::GetQueueSize() const {
    return GetCurrentSize();
}

QueueStats DefaultMpscQueue::GetStats() const {
    return stats_;
}

void DefaultMpscQueue::ResetStats() {
    stats_.totalEnqueued.store(0);
    stats_.totalDequeued.store(0);
    stats_.totalProcessed.store(0);
    stats_.totalCompleted.store(0);
    stats_.totalFailed.store(0);
    stats_.totalCancelled.store(0);
    stats_.currentQueueSize.store(0);
    stats_.maxQueueSize.store(0);
    stats_.totalProcessingTime.store(0);
    
    Logger::Info("DefaultMpscQueue::ResetStats - 统计信息已重置");
}

// === 私有辅助方法实现 ===

uint64_t DefaultMpscQueue::enqueueByPriority(const WorkItem& workItem) {
    WorkItem newWorkItem = workItem;
    newWorkItem.id = GenerateWorkId();
    newWorkItem.state = WorkItemState::Pending;
    newWorkItem.timestamp = GetCurrentTimestamp();
    
    // 根据优先级选择队列
    uint32_t priorityIndex = static_cast<uint32_t>(workItem.priority);
    if (priorityIndex >= PRIORITY_LEVELS) {
        priorityIndex = PRIORITY_LEVELS - 1; // 限制在有效范围内
    }
    
    // 入队到对应优先级队列
    bool success = priorityQueues_[priorityIndex].enqueue(newWorkItem);
    
    if (success) {
        // 更新状态映射
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            workItemStates_[newWorkItem.id] = WorkItemState::Pending;
        }
        
        // 更新统计信息
        stats_.totalEnqueued.fetch_add(1);
        stats_.currentQueueSize.fetch_add(1);
        UpdateMaxQueueSize();
        
        Logger::Debug("DefaultMpscQueue::enqueueByPriority - 工作项入队成功: ID={}, Priority={}", 
                     newWorkItem.id, static_cast<uint32_t>(workItem.priority));
        return newWorkItem.id;
    } else {
        Logger::Error("DefaultMpscQueue::enqueueByPriority - 入队失败");
        return 0;
    }
}

bool DefaultMpscQueue::dequeueByPriority(WorkItem& workItem) {
    // 按优先级顺序尝试出队（Critical -> High -> Normal -> Low）
    for (int32_t i = PRIORITY_LEVELS - 1; i >= 0; --i) {
        if (priorityQueues_[i].try_dequeue(workItem)) {
            // 更新状态
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                workItemStates_[workItem.id] = WorkItemState::Processing;
            }
            
            // 更新统计信息
            stats_.totalDequeued.fetch_add(1);
            stats_.currentQueueSize.fetch_sub(1);
            
            Logger::Debug("DefaultMpscQueue::dequeueByPriority - 工作项出队成功: ID={}, Priority={}", 
                         workItem.id, static_cast<uint32_t>(workItem.priority));
            return true;
        }
    }
    
    return false;
}

void DefaultMpscQueue::workerThreadFunc() {
    Logger::Info("DefaultMpscQueue::workerThreadFunc - 工作线程启动");
    
    while (!shutdownRequested_.load()) {
        WorkItem workItem;
        
        // 尝试获取工作项
        if (dequeueByPriority(workItem)) {
            auto startTime = GetCurrentTimestamp();
            
            try {
                // 处理工作项
                processWorkItem(workItem);
                
                // 更新状态为完成
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    workItemStates_[workItem.id] = WorkItemState::Completed;
                }
                
                stats_.totalCompleted.fetch_add(1);
                
                // 通知等待的线程
                auto condIt = workConditions_.find(workItem.id);
                if (condIt != workConditions_.end() && condIt->second) {
                    condIt->second->notify_all();
                }
            }
            catch (const std::exception& e) {
                Logger::Error("DefaultMpscQueue::workerThreadFunc - 处理工作项异常: {}", e.what());
                
                // 更新状态为失败
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    workItemStates_[workItem.id] = WorkItemState::Failed;
                }
                
                stats_.totalFailed.fetch_add(1);
                
                // 通知等待的线程
                auto condIt = workConditions_.find(workItem.id);
                if (condIt != workConditions_.end() && condIt->second) {
                    condIt->second->notify_all();
                }
            }
            
            auto endTime = GetCurrentTimestamp();
            uint64_t processingTime = endTime - startTime;
            stats_.totalProcessingTime.fetch_add(processingTime);
            stats_.totalProcessed.fetch_add(1);
            
            // 调用完成回调
            if (workItem.completionCallback) {
                workItem.completionCallback();
            }
        } else {
            // 没有工作项时短暂休眠
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    Logger::Info("DefaultMpscQueue::workerThreadFunc - 工作线程退出");
}

void DefaultMpscQueue::processWorkItem(const WorkItem& workItem) {
    try {
        switch (workItem.type) {
            case WorkItemType::CommandBuffer:
                processCommandBuffer(workItem);
                break;
                
            case WorkItemType::ResourceUpdate:
                processResourceUpdate(workItem);
                break;
                
            case WorkItemType::MemoryOperation:
                processMemoryOperation(workItem);
                break;
                
            case WorkItemType::SyncOperation:
                processSyncOperation(workItem);
                break;
                
            case WorkItemType::CustomCallback:
                processCustomCallback(workItem);
                break;
                
            default:
                Logger::Warn("DefaultMpscQueue::processWorkItem - 未知工作项类型: {}", static_cast<uint8_t>(workItem.type));
                break;
        }
    }
    catch (const std::exception& e) {
        Logger::Error("DefaultMpscQueue::processWorkItem - 处理工作项时发生异常: {}", e.what());
        throw;
    }
}

void DefaultMpscQueue::processCommandBuffer(const WorkItem& workItem) {
    Logger::Debug("DefaultMpscQueue::processCommandBuffer - 处理命令缓冲区工作项，ID: {}", workItem.id);
    
    // TODO: 调用RHI命令系统的相关接口
    // 例如：提交命令缓冲区到GPU
}

void DefaultMpscQueue::processResourceUpdate(const WorkItem& workItem) {
    Logger::Debug("DefaultMpscQueue::processResourceUpdate - 处理资源更新工作项，ID: {}", workItem.id);
    
    // TODO: 调用RHI资源系统的相关接口
    // 例如：更新纹理数据、缓冲区数据等
}

void DefaultMpscQueue::processMemoryOperation(const WorkItem& workItem) {
    Logger::Debug("DefaultMpscQueue::processMemoryOperation - 处理内存操作工作项，ID: {}", workItem.id);
    
    // TODO: 调用RHI内存池系统的相关接口
    // 例如：分配GPU内存、释放GPU内存等
}

void DefaultMpscQueue::processSyncOperation(const WorkItem& workItem) {
    Logger::Debug("DefaultMpscQueue::processSyncOperation - 处理同步操作工作项，ID: {}", workItem.id);
    
    // TODO: 调用RHI同步系统的相关接口
    // 例如：创建Fence、等待Fence等
}

void DefaultMpscQueue::processCustomCallback(const WorkItem& workItem) {
    Logger::Debug("DefaultMpscQueue::processCustomCallback - 处理自定义回调工作项，ID: {}", workItem.id);
    
    // 调用用户自定义的回调函数
    if (workItem.callbackData.callback) {
        workItem.callbackData.callback();
    }
}

// === 工厂函数实现 ===

std::unique_ptr<RHIMpscQueue> MpscQueueFactory::CreateQueue(RHIDevice& device, const QueueConfig& config) {
    auto queue = std::make_unique<DefaultMpscQueue>(device, config);
    
    if (!queue->Initialize()) {
        Logger::Error("MpscQueueFactory::CreateQueue - 创建MPSC队列失败");
        return nullptr;
    }
    
    return queue;
}

QueueConfig MpscQueueFactory::GetRecommendedConfig(const char* usage, const char* expectedLoad) {
    QueueConfig config;
    
    // 根据使用场景推荐配置
    if (strcmp(usage, "rendering") == 0) {
        config.maxQueueSize = 2048;
        config.batchSize = 32;
        config.enablePriorityQueue = true;
        config.enableTimeout = false;
    } else if (strcmp(usage, "resource_loading") == 0) {
        config.maxQueueSize = 512;
        config.batchSize = 16;
        config.enablePriorityQueue = false;
        config.enableTimeout = true;
        config.defaultTimeoutMs = 10000;
    } else {
        // 默认配置
        config.maxQueueSize = 1024;
        config.batchSize = 32;
        config.enablePriorityQueue = true;
        config.enableTimeout = true;
        config.defaultTimeoutMs = 5000;
    }
    
    // 根据预期负载调整配置
    if (strcmp(expectedLoad, "high") == 0) {
        config.maxQueueSize *= 2;
        config.batchSize *= 2;
    } else if (strcmp(expectedLoad, "low") == 0) {
        config.maxQueueSize /= 2;
        config.batchSize /= 2;
    }
    
    config.workerThreadCount = 1;
    config.enableStatistics = true;
    config.name = usage;
    
    return config;
}

} // namespace primal::graphics::rhi