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
#include "RHIDebug.h"
#include "../../../../third_party/moodycamel-ConcurrentQueue/concurrentqueue.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <condition_variable>

namespace primal::graphics::rhi {

// === RHIMpscQueue基类方法实现 ===

RHIMpscQueue& RHIMpscQueue::operator=(RHIMpscQueue&& other) noexcept {
    if (this != &other) {
        if (running_) {
            Stop();
        }
        
        device_ = other.device_;
        config_ = std::move(other.config_);
        stats_ = other.stats_;
        running_.store(other.running_.load());
        nextWorkId_.store(other.nextWorkId_.load());
        
        other.running_ = false;
        other.nextWorkId_ = 1;
        other.stats_ = QueueStats{};
    }
    return *this;
}

// === DefaultMpscQueue 实现 ===

DefaultMpscQueue::DefaultMpscQueue(RHIDeviceBase& device, const QueueConfig& config)
    : RHIMpscQueue(device, config)
    , workQueue_()
    , workerThread_()
    , shutdownRequested_(false)
    , initialized_(false) {
    
    // 预分配优先级队列
    for (u32 i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
}

DefaultMpscQueue::~DefaultMpscQueue() {
    if (running_.load()) {
        stopImpl();
        running_ = false;
    }
}

bool DefaultMpscQueue::Initialize() {
    if (initialized_) {
        return true;
    }

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
    
    return true;
}

bool DefaultMpscQueue::Start() {
    if (!initialized_) {
        return false;
    }

    if (running_.load()) {
        return true;
    }

    shutdownRequested_.store(false);
    
    // 启动工作线程
    workerThread_ = std::thread(&DefaultMpscQueue::workerThreadFunc, this);
    
    running_.store(true);
    
    return true;
}

void DefaultMpscQueue::stopImpl() {
    shutdownRequested_.store(true);
    
    // 唤醒所有等待的消费者线程
    WakeUpAllConsumers();
    
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    
}

void DefaultMpscQueue::WakeUpAllConsumers() {
    // 通知所有等待的线程
    conditionVariable_.notify_all();
}

void DefaultMpscQueue::destroyImpl() {
    // 清空所有队列
    workQueue_ = moodycamel::ConcurrentQueue<WorkItem>();
    
    for (u32 i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
    
    // 清空工作项状态映射
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        workItemStates_.clear();
        workConditions_.clear();
    }
    
    initialized_.store(false);
    
}

u64 DefaultMpscQueue::Enqueue(const WorkItem& workItem) {
    if (!initialized_ || !running_.load()) {
        return 0;
    }

    // 检查队列大小限制
    u32 currentSize = GetCurrentSize();
    if (currentSize >= config_.maxQueueSize) {
        stats_.totalFailed.fetch_add(1);
        return 0;
    }

    return enqueueByPriority(workItem);
}

u32 DefaultMpscQueue::EnqueueBatch(const WorkItem* workItems, u32 count) {
    if (!initialized_ || !running_.load() || !workItems) {
        return 0;
    }

    u32 successCount = 0;
    u32 currentSize = GetCurrentSize();
    
    for (u32 i = 0; i < count && currentSize < config_.maxQueueSize; ++i) {
        if (enqueueByPriority(workItems[i]) != 0) {
            successCount++;
            currentSize++;
        }
    }
    
    return successCount;
}

bool DefaultMpscQueue::Dequeue(WorkItem& workItem) {
    if (!initialized_) {
        return false;
    }

    return dequeueByPriority(workItem);
}

u32 DefaultMpscQueue::DequeueBatch(WorkItem* workItems, u32 maxCount) {
    if (!initialized_ || !workItems || maxCount == 0) {
        return 0;
    }

    u32 dequeuedCount = 0;
    
    for (u32 i = 0; i < maxCount; ++i) {
        if (!dequeueByPriority(workItems[i])) {
            break;
        }
        dequeuedCount++;
    }
    
    return dequeuedCount;
}

bool DefaultMpscQueue::CancelWork(u64 workId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    auto it = workItemStates_.find(workId);
    if (it != workItemStates_.end() && it->second == WorkItemState::Pending) {
        it->second = WorkItemState::Cancelled;
        stats_.totalCancelled.fetch_add(1);
        
        return true;
    }
    
    return false;
}

WorkItemState DefaultMpscQueue::GetWorkState(u64 workId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    
    auto it = workItemStates_.find(workId);
    if (it != workItemStates_.end()) {
        return it->second;
    }
    
    return WorkItemState::Pending;
}

bool DefaultMpscQueue::WaitForWork(u64 workId, u32 timeoutMs) {
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
    u64 processed = stats_.totalProcessed.load();
    u64 completed = stats_.totalCompleted.load();
    u64 failed = stats_.totalFailed.load();
    u64 cancelled = stats_.totalCancelled.load();
    
    // 处理的数量应该等于完成+失败+取消的数量
    if (processed != (completed + failed + cancelled)) {
                     
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
    
    for (u32 i = 0; i < PRIORITY_LEVELS; ++i) {
        priorityQueues_[i] = moodycamel::ConcurrentQueue<WorkItem>();
    }
    
    // 更新统计信息
    u32 currentSize = stats_.currentQueueSize.load();
    stats_.totalDequeued.fetch_add(currentSize);
    stats_.currentQueueSize.store(0);
    
}

bool DefaultMpscQueue::IsEmpty() const {
    // 检查所有队列是否为空
    if (workQueue_.size_approx() > 0) {
        return false;
    }
    
    for (u32 i = 0; i < PRIORITY_LEVELS; ++i) {
        if (priorityQueues_[i].size_approx() > 0) {
            return false;
        }
    }
    
    return true;
}

u32 DefaultMpscQueue::GetQueueSize() const {
    return GetCurrentSize();
}

QueueStats DefaultMpscQueue::GetStats() const {
    return QueueStats {
        stats_.totalEnqueued.load(),
        stats_.totalDequeued.load(),
        stats_.totalProcessed.load(),
        stats_.totalCompleted.load(),
        stats_.totalFailed.load(),
        stats_.totalCancelled.load(),
        stats_.currentQueueSize.load(),
        stats_.maxQueueSize.load(),
        stats_.totalProcessingTime.load()
    };
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
    
}

// === 私有辅助方法实现 ===

u64 DefaultMpscQueue::enqueueByPriority(const WorkItem& workItem) {
    WorkItem newWorkItem = workItem;
    newWorkItem.id = GenerateWorkId();
    newWorkItem.state = WorkItemState::Pending;
    newWorkItem.timestamp = GetCurrentTimestamp();
    
    // 根据优先级选择队列
    u32 priorityIndex = static_cast<u32>(workItem.priority);
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
        
        return newWorkItem.id;
    } else {
        return 0;
    }
}

bool DefaultMpscQueue::dequeueByPriority(WorkItem& workItem) {
    // 按优先级顺序尝试出队（Critical -> High -> Normal -> Low）
    for (s32 i = PRIORITY_LEVELS - 1; i >= 0; --i) {
        if (priorityQueues_[i].try_dequeue(workItem)) {
            // 更新状态
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                workItemStates_[workItem.id] = WorkItemState::Processing;
            }
            
            // 更新统计信息
            stats_.totalDequeued.fetch_add(1);
            stats_.currentQueueSize.fetch_sub(1);
            
            return true;
        }
    }
    
    return false;
}

void DefaultMpscQueue::workerThreadFunc() {
    
    while (!shutdownRequested_.load()) {
        WorkItem workItem;
        bool hasWorkItem = false;
        auto startTime = GetCurrentTimestamp();
        
        // 尝试获取工作项
        if (dequeueByPriority(workItem)) {
            hasWorkItem = true;
            
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
            
            // 调用完成回调
            if (workItem.completionCallback) {
                (*workItem.completionCallback)();
            }
        } else {
            // 没有工作项时短暂休眠
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        if (hasWorkItem) {
            auto endTime = GetCurrentTimestamp();
            u64 processingTime = endTime - startTime;
            stats_.totalProcessingTime.fetch_add(processingTime);
            stats_.totalProcessed.fetch_add(1);
        }
    }
    
}

void primal::graphics::rhi::DefaultMpscQueue::processWorkItem(const WorkItem& workItem) {
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
            break;
    }
}

void primal::graphics::rhi::DefaultMpscQueue::processCommandBuffer(const WorkItem& workItem) {
    
    // TODO: 调用RHI命令系统的相关接口
    // 例如：提交命令缓冲区到GPU
}

void primal::graphics::rhi::DefaultMpscQueue::processResourceUpdate(const WorkItem& workItem) {
    
    // TODO: 调用RHI资源系统的相关接口
    // 例如：更新纹理数据、缓冲区数据等
}

void primal::graphics::rhi::DefaultMpscQueue::processMemoryOperation(const WorkItem& workItem) {
    
    // TODO: 调用RHI内存池系统的相关接口
    // 例如：分配GPU内存、释放GPU内存等
}

void primal::graphics::rhi::DefaultMpscQueue::processSyncOperation(const WorkItem& workItem) {
    
    // TODO: 调用RHI同步系统的相关接口
    // 例如：创建Fence、等待Fence等
}

void primal::graphics::rhi::DefaultMpscQueue::processCustomCallback(const WorkItem& workItem) {
    
    // 调用用户自定义的回调函数
    if (workItem.callbackData.callback) {
        (*workItem.callbackData.callback)();
    }
}

// === 工厂函数实现 ===

std::unique_ptr<primal::graphics::rhi::RHIMpscQueue> primal::graphics::rhi::MpscQueueFactory::CreateQueue(RHIDeviceBase& device, const QueueConfig& config) {
    auto queue = std::make_unique<DefaultMpscQueue>(device, config);
    
    if (!queue->Initialize()) {
        return nullptr;
    }
    
    return queue;
}

primal::graphics::rhi::QueueConfig primal::graphics::rhi::MpscQueueFactory::GetRecommendedConfig(const char* usage, const char* expectedLoad) {
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