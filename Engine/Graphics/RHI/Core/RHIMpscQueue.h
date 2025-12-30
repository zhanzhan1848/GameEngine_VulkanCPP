/**
 * @file RHIMpscQueue.h
 * @brief RHI多生产者单消费者(MPSC)工作队列系统
 * @details 提供线程安全的高性能工作队列，用于多线程向RHI提交工作
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "RHITypes.h"
#include "RHICommand.h"
#include "../../../Utilities/Vector.h"
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <unordered_map>

// 集成moodycamel::ConcurrentQueue
#include "../../../../third_party/moodycamel-ConcurrentQueue/concurrentqueue.h"

namespace primal::graphics::rhi {

// === 前向声明 ===

class RHIDevice;

/**
 * @brief 工作项类型枚举
 * @details 定义不同类型的工作项
 */
enum class WorkItemType : uint8_t {
    Unknown = 0,
    CommandBuffer = 1,     ///< 命令缓冲区工作项
    ResourceUpdate = 2,    ///< 资源更新工作项
    MemoryOperation = 3,   ///< 内存操作工作项
    SyncOperation = 4,     ///< 同步操作工作项
    CustomCallback = 5      ///< 自定义回调工作项
};

/**
 * @brief 工作项优先级枚举
 * @details 定义工作项的执行优先级
 */
enum class WorkPriority : uint8_t {
    Low = 0,              ///< 低优先级
    Normal = 1,           ///< 普通优先级
    High = 2,             ///< 高优先级
    Critical = 3          ///< 关键优先级
};

/**
 * @brief 工作项状态枚举
 */
enum class WorkItemState : uint8_t {
    Pending = 0,          ///< 等待执行
    Processing = 1,       ///< 正在处理
    Completed = 2,        ///< 已完成
    Failed = 3,           ///< 执行失败
    Cancelled = 4          ///< 已取消
};

/**
 * @brief 工作项描述符
 * @details 包含工作项的所有必要信息
 */
struct WorkItem {
    WorkItemType type;            ///< 工作项类型
    WorkPriority priority;        ///< 优先级
    WorkItemState state;          ///< 状态
    uint64_t id;                  ///< 工作项ID
    uint64_t timestamp;           ///< 创建时间戳
    uint32_t timeoutMs;           ///< 超时时间（毫秒）
    
    union {
        struct {
            CommandBufferHandle commandBuffer;  ///< 命令缓冲区句柄
            uint32_t waitFlags;                 ///< 等待标志
        } commandData;
        
        struct {
            ResourceHandle resource;            ///< 资源句柄
            void* data;                          ///< 数据指针
            uint64_t dataSize;                   ///< 数据大小
            uint64_t offset;                      ///< 偏移量
        } resourceData;
        
        struct {
            void* srcPtr;                        ///< 源指针
            void* dstPtr;                        ///< 目标指针
            uint64_t size;                       ///< 复制大小
        } memoryData;
        
        struct {
            void* userData;                      ///< 用户数据
            std::function<void()> callback;       ///< 回调函数
        } callbackData;
    };
    
    void* context;                ///< 上下文数据
    std::function<void()> completionCallback;  ///< 完成回调
    
    WorkItem() : type(WorkItemType::Unknown), priority(WorkPriority::Normal), 
                state(WorkItemState::Pending), id(0), timestamp(0), timeoutMs(0), context(nullptr) {}
};

/**
 * @brief 队列统计信息
 * @details 用于性能分析和调试
 */
struct QueueStats {
    std::atomic<uint64_t> totalEnqueued;      ///< 总入队数量
    std::atomic<uint64_t> totalDequeued;      ///< 总出队数量
    std::atomic<uint64_t> totalProcessed;      ///< 总处理数量
    std::atomic<uint64_t> totalCompleted;      ///< 总完成数量
    std::atomic<uint64_t> totalFailed;         ///< 总失败数量
    std::atomic<uint64_t> totalCancelled;      ///< 总取消数量
    
    std::atomic<uint32_t> currentQueueSize;    ///< 当前队列大小
    std::atomic<uint32_t> maxQueueSize;        ///< 最大队列大小
    std::atomic<uint64_t> totalProcessingTime; ///< 总处理时间（微秒）
    
    QueueStats() {
        totalEnqueued.store(0);
        totalDequeued.store(0);
        totalProcessed.store(0);
        totalCompleted.store(0);
        totalFailed.store(0);
        totalCancelled.store(0);
        currentQueueSize.store(0);
        maxQueueSize.store(0);
        totalProcessingTime.store(0);
    }
    
    QueueStats& operator=(const QueueStats& other) {
        if (this != &other) {
            totalEnqueued.store(other.totalEnqueued.load());
            totalDequeued.store(other.totalDequeued.load());
            totalProcessed.store(other.totalProcessed.load());
            totalCompleted.store(other.totalCompleted.load());
            totalFailed.store(other.totalFailed.load());
            totalCancelled.store(other.totalCancelled.load());
            currentQueueSize.store(other.currentQueueSize.load());
            maxQueueSize.store(other.maxQueueSize.load());
            totalProcessingTime.store(other.totalProcessingTime.load());
        }
        return *this;
    }
};

/**
 * @brief 队列配置参数
 */
struct QueueConfig {
    uint32_t maxQueueSize;          ///< 最大队列大小
    uint32_t batchSize;             ///< 批处理大小
    uint32_t workerThreadCount;    ///< 工作线程数量
    bool enablePriorityQueue;       ///< 是否启用优先级队列
    bool enableTimeout;             ///< 是否启用超时机制
    uint32_t defaultTimeoutMs;     ///< 默认超时时间
    bool enableStatistics;          ///< 是否启用统计信息
    const char* name;               ///< 队列名称
    
    QueueConfig() : maxQueueSize(10000), batchSize(32), workerThreadCount(1),
                    enablePriorityQueue(true), enableTimeout(true), defaultTimeoutMs(5000),
                    enableStatistics(true), name("RHIMpscQueue") {}
};

/**
 * @brief MPSC队列基类
 * @details 多生产者单消费者队列的抽象基类
 */
class RHIMpscQueue {
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param config 队列配置
     */
    explicit RHIMpscQueue(RHIDevice& device, const QueueConfig& config)
        : device_(device), config_(config), stats_(), 
          running_(false), nextWorkId_(1) {}
    
    /**
     * @brief 虚析构函数
     */
    virtual ~RHIMpscQueue() {
        if (running_) {
            Stop();
        }
    }
    
    // === 禁用拷贝，支持移动 ===
    
    RHIMpscQueue(const RHIMpscQueue&) = delete;
    RHIMpscQueue& operator=(const RHIMpscQueue&) = delete;
    
    RHIMpscQueue(RHIMpscQueue&& other) noexcept
        : device_(other.device_), config_(std::move(other.config_)), stats_(other.stats_),
          running_(other.running_.load()), nextWorkId_(other.nextWorkId_.load()) {
        other.running_ = false;
        other.nextWorkId_ = 1;
    }
    
    RHIMpscQueue& operator=(RHIMpscQueue&& other) noexcept {
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
        }
        return *this;
    }
    
    // === 核心接口方法 ===
    
    /**
     * @brief 初始化队列
     * @return 初始化是否成功
     */
    virtual bool Initialize() = 0;
    
    /**
     * @brief 启动队列处理
     * @return 启动是否成功
     */
    virtual bool Start() = 0;
    
    /**
     * @brief 停止队列处理
     */
    virtual void Stop() {
        if (running_) {
            stopImpl();
            running_ = false;
        }
    }
    
    /**
     * @brief 销毁队列
     */
    virtual void Destroy() {
        Stop();
        destroyImpl();
    }
    
    // === 工作项操作 ===
    
    /**
     * @brief 提交工作项
     * @param workItem 工作项
     * @return 工作项ID，失败返回0
     */
    virtual uint64_t Enqueue(const WorkItem& workItem) = 0;
    
    /**
     * @brief 批量提交工作项
     * @param workItems 工作项数组
     * @param count 数组大小
     * @return 成功提交的数量
     */
    virtual uint32_t EnqueueBatch(const WorkItem* workItems, uint32_t count) = 0;
    
    /**
     * @brief 获取下一个工作项
     * @param workItem 输出工作项
     * @return 是否获取成功
     */
    virtual bool Dequeue(WorkItem& workItem) = 0;
    
    /**
     * @brief 批量获取工作项
     * @param workItems 输出工作项数组
     * @param maxCount 最大数量
     * @return 实际获取的数量
     */
    virtual uint32_t DequeueBatch(WorkItem* workItems, uint32_t maxCount) = 0;
    
    /**
     * @brief 取消工作项
     * @param workId 工作项ID
     * @return 取消是否成功
     */
    virtual bool CancelWork(uint64_t workId) = 0;
    
    /**
     * @brief 获取工作项状态
     * @param workId 工作项ID
     * @return 工作项状态
     */
    virtual WorkItemState GetWorkState(uint64_t workId) const = 0;
    
    /**
     * @brief 等待工作项完成
     * @param workId 工作项ID
     * @param timeoutMs 超时时间
     * @return 是否完成
     */
    virtual bool WaitForWork(uint64_t workId, uint32_t timeoutMs = 0) = 0;
    
    // === 便利方法 ===
    
    /**
     * @brief 提交命令缓冲区
     * @param commandBuffer 命令缓冲区句柄
     * @param priority 优先级
     * @param waitFlags 等待标志
     * @param completionCallback 完成回调
     * @return 工作项ID
     */
    uint64_t SubmitCommandBuffer(CommandBufferHandle commandBuffer, 
                                WorkPriority priority = WorkPriority::Normal,
                                uint32_t waitFlags = 0,
                                std::function<void()> completionCallback = nullptr);
    
    /**
     * @brief 提交资源更新
     * @param resource 资源句柄
     * @param data 数据指针
     * @param dataSize 数据大小
     * @param offset 偏移量
     * @param priority 优先级
     * @param completionCallback 完成回调
     * @return 工作项ID
     */
    uint64_t SubmitResourceUpdate(ResourceHandle resource, void* data, uint64_t dataSize,
                                  uint64_t offset = 0, WorkPriority priority = WorkPriority::Normal,
                                  std::function<void()> completionCallback = nullptr);
    
    /**
     * @brief 提交自定义回调
     * @param callback 回调函数
     * @param userData 用户数据
     * @param priority 优先级
     * @param completionCallback 完成回调
     * @return 工作项ID
     */
    uint64_t SubmitCallback(std::function<void()> callback, void* userData = nullptr,
                            WorkPriority priority = WorkPriority::Normal,
                            std::function<void()> completionCallback = nullptr);
    
    // === 访问器方法 ===
    
    /**
     * @brief 检查是否正在运行
     * @return 运行状态
     */
    bool IsRunning() const { return running_; }
    
    /**
     * @brief 获取队列配置
     * @return 配置的常量引用
     */
    const QueueConfig& GetConfig() const { return config_; }
    
    /**
     * @brief 获取统计信息
     * @return 统计信息的常量引用
     */
    const QueueStats& GetStats() const { return stats_; }
    
    /**
     * @brief 获取当前队列大小
     * @return 当前队列大小
     */
    uint32_t GetCurrentSize() const { return stats_.currentQueueSize.load(); }
    
    /**
     * @brief 检查队列是否为空
     * @return 是否为空
     */
    bool IsEmpty() const { return stats_.currentQueueSize.load() == 0; }
    
    /**
     * @brief 检查队列是否已满
     * @return 是否已满
     */
    bool IsFull() const {
        return stats_.currentQueueSize.load() >= config_.maxQueueSize;
    }
    
    /**
     * @brief 获取队列使用率
     * @return 使用率（0.0 - 1.0）
     */
    float GetUsageRatio() const {
        return static_cast<float>(stats_.currentQueueSize.load()) / 
               static_cast<float>(config_.maxQueueSize);
    }
    
    // === 调试和诊断方法 ===
    
    /**
     * @brief 打印队列统计信息
     */
    void PrintStats() const;
    
    /**
     * @brief 重置统计信息
     */
    void ResetStats();
    
    /**
     * @brief 生成队列报告
     * @return 报告字符串
     */
    std::string GenerateReport() const;
    
    /**
     * @brief 验证队列一致性
     * @return 验证是否通过
     */
    virtual bool Validate() const = 0;

protected:
    // === 受保护的虚函数 ===
    
    virtual void stopImpl() = 0;
    virtual void destroyImpl() = 0;
    virtual void processWorkItem(const WorkItem& workItem) = 0;
    
    // === 受保护的成员变量 ===
    
    RHIDevice& device_;                 ///< 设备引用
    QueueConfig config_;                ///< 队列配置
    QueueStats stats_;                  ///< 统计信息
    std::atomic<bool> running_;         ///< 运行状态
    std::atomic<uint64_t> nextWorkId_;  ///< 下一个工作项ID
    
    // === 受保护的辅助方法 ===
    
    /**
     * @brief 生成下一个工作项ID
     * @return 工作项ID
     */
    uint64_t GenerateWorkId() {
        return nextWorkId_.fetch_add(1);
    }
    
    /**
     * @brief 更新统计信息
     */
    void UpdateMaxQueueSize() {
        uint32_t currentSize = stats_.currentQueueSize.load();
        uint32_t maxSize = stats_.maxQueueSize.load();
        while (currentSize > maxSize) {
            if (stats_.maxQueueSize.compare_exchange_weak(maxSize, currentSize)) {
                break;
            }
        }
    }
    
    /**
     * @brief 获取当前时间戳（微秒）
     * @return 时间戳
     */
    static uint64_t GetCurrentTimestamp() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }
    
    /**
     * @brief 检查工作项是否超时
     * @param workItem 工作项
     * @return 是否超时
     */
    bool IsWorkItemTimeout(const WorkItem& workItem) const {
        if (!config_.enableTimeout || workItem.timeoutMs == 0) {
            return false;
        }
        
        uint64_t currentTime = GetCurrentTimestamp();
        uint64_t elapsed = currentTime - workItem.timestamp;
        return elapsed > (workItem.timeoutMs * 1000);  // 转换为微秒
    }
    
    /**
     * @brief 处理工作项完成
     * @param workItem 工作项
     * @param success 是否成功
     */
    void HandleWorkItemCompleted(const WorkItem& workItem, bool success) {
        stats_.totalProcessed.fetch_add(1);
        
        if (success) {
            stats_.totalCompleted.fetch_add(1);
        } else {
            stats_.totalFailed.fetch_add(1);
        }
        
        // 调用完成回调
        if (workItem.completionCallback) {
            workItem.completionCallback();
        }
    }
    
    /**
     * @brief 线程安全锁（如果需要）
     */
    class ScopedLock {
    public:
        explicit ScopedLock(std::mutex& mutex) : mutex_(mutex) {
            mutex_.lock();
        }
        
        ~ScopedLock() {
            mutex_.unlock();
        }
        
    private:
        std::mutex& mutex_;
    };
};

// === 默认MPSC队列实现 ===

/**
 * @brief 基于moodycamel::ConcurrentQueue的默认MPSC队列实现
 * @details 使用工业级无锁队列实现高性能多生产者单消费者工作队列
 */
class DefaultMpscQueue : public RHIMpscQueue {
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param config 队列配置
     */
    explicit DefaultMpscQueue(RHIDevice& device, const QueueConfig& config);
    
    /**
     * @brief 析构函数
     */
    ~DefaultMpscQueue() override;
    
    // === 核心接口实现 ===
    
    /**
     * @brief 初始化队列
     * @return 初始化是否成功
     */
    bool Initialize() override;
    
    /**
     * @brief 启动队列处理
     * @return 启动是否成功
     */
    bool Start() override;
    
    /**
     * @brief 提交工作项
     * @param workItem 工作项
     * @return 工作项ID，失败返回0
     */
    uint64_t Enqueue(const WorkItem& workItem) override;
    
    /**
     * @brief 批量提交工作项
     * @param workItems 工作项数组
     * @param count 数组大小
     * @return 成功提交的数量
     */
    uint32_t EnqueueBatch(const WorkItem* workItems, uint32_t count) override;
    
    /**
     * @brief 获取下一个工作项
     * @param workItem 输出工作项
     * @return 是否获取成功
     */
    bool Dequeue(WorkItem& workItem) override;
    
    /**
     * @brief 批量获取工作项
     * @param workItems 输出工作项数组
     * @param maxCount 最大数量
     * @return 实际获取的数量
     */
    uint32_t DequeueBatch(WorkItem* workItems, uint32_t maxCount) override;
    
    /**
     * @brief 取消工作项
     * @param workId 工作项ID
     * @return 取消是否成功
     */
    bool CancelWork(uint64_t workId) override;
    
    /**
     * @brief 获取工作项状态
     * @param workId 工作项ID
     * @return 工作项状态
     */
    WorkItemState GetWorkState(uint64_t workId) const override;
    
    /**
     * @brief 等待工作项完成
     * @param workId 工作项ID
     * @param timeoutMs 超时时间
     * @return 是否完成
     */
    bool WaitForWork(uint64_t workId, uint32_t timeoutMs = 0) override;
    
    /**
     * @brief 验证队列一致性
     * @return 验证是否通过
     */
    bool Validate() const override;
    
    // === 扩展方法 ===
    
    /**
     * @brief 尝试获取工作项（非阻塞）
     * @param workItem 输出工作项
     * @return 是否获取成功
     */
    bool TryDequeue(WorkItem& workItem);
    
    /**
     * @brief 等待队列为空
     */
    void WaitForIdle();
    
    /**
     * @brief 清空队列
     */
    void Clear();
    
    /**
     * @brief 检查队列是否为空
     * @return 是否为空
     */
    bool IsEmpty() const;
    
    /**
     * @brief 获取当前队列大小
     * @return 队列大小
     */
    uint32_t GetQueueSize() const;
    
    /**
     * @brief 获取统计信息
     * @return 统计信息
     */
    QueueStats GetStats() const;
    
    /**
     * @brief 重置统计信息
     */
    void ResetStats();

private:
    // === 受保护的虚函数实现 ===
    
    void stopImpl() override;
    void destroyImpl() override;
    void processWorkItem(const WorkItem& workItem) override;
    
    // === 私有成员变量 ===
    
    moodycamel::ConcurrentQueue<WorkItem> workQueue_;     ///< moodycamel无锁队列
    std::thread workerThread_;                            ///< 工作线程
    std::atomic<bool> shutdownRequested_;                 ///< 关闭请求标志
    std::atomic<bool> initialized_;                       ///< 初始化标志
    
    // 优先级队列 - 使用多个队列实现优先级
    static constexpr uint32_t PRIORITY_LEVELS = 4;        ///< 优先级层级数
    moodycamel::ConcurrentQueue<WorkItem> priorityQueues_[PRIORITY_LEVELS]; ///< 优先级队列数组
    
    // 工作项状态跟踪（用于状态查询和等待）
    mutable std::unordered_map<uint64_t, WorkItemState> workItemStates_;    ///< 工作项状态映射
    mutable std::mutex stateMutex_;                   ///< 状态映射的互斥锁
    std::unordered_map<uint64_t, std::condition_variable*> workConditions_; ///< 工作项条件变量
    
    // === 私有辅助方法 ===
    
    /**
     * @brief 工作线程函数
     */
    void workerThreadFunc();
    
    /**
     * @brief 根据优先级入队
     * @param workItem 工作项
     * @return 工作项ID
     */
    uint64_t enqueueByPriority(const WorkItem& workItem);
    
    /**
     * @brief 根据优先级出队
     * @param workItem 输出工作项
     * @return 是否获取成功
     */
    bool dequeueByPriority(WorkItem& workItem);
    
    /**
     * @brief 处理命令缓冲区工作项
     * @param workItem 工作项
     */
    void processCommandBuffer(const WorkItem& workItem);
    
    /**
     * @brief 处理资源更新工作项
     * @param workItem 工作项
     */
    void processResourceUpdate(const WorkItem& workItem);
    
    /**
     * @brief 处理内存操作工作项
     * @param workItem 工作项
     */
    void processMemoryOperation(const WorkItem& workItem);
    
    /**
     * @brief 处理同步操作工作项
     * @param workItem 工作项
     */
    void processSyncOperation(const WorkItem& workItem);
    
    /**
     * @brief 处理自定义回调工作项
     * @param workItem 工作项
     */
    void processCustomCallback(const WorkItem& workItem);
};

// === 工作队列工厂 ===

/**
 * @brief 工作队列工厂类
 * @details 创建不同类型的工作队列实例
 */
class MpscQueueFactory {
public:
    /**
     * @brief 创建MPSC队列
     * @param device 设备引用
     * @param config 队列配置
     * @return 队列指针，失败返回nullptr
     */
    static std::unique_ptr<RHIMpscQueue> CreateQueue(RHIDevice& device, const QueueConfig& config);
    
    /**
     * @brief 获取推荐的队列配置
     * @param usage 使用场景
     * @param expectedLoad 预期负载
     * @return 推荐的配置
     */
    static QueueConfig GetRecommendedConfig(const char* usage, const char* expectedLoad);
};

} // namespace primal::graphics::rhi