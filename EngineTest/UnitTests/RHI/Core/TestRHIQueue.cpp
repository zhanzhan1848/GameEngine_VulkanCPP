/**
 * @file TestRHIQueue.cpp
 * @brief RHI命令队列功能测试
 * @details 测试RHI命令队列的创建、命令缓冲区管理、同步等功能
 * 
 * @author zhanyuanwei
 * @date 2025-12-30
 * @version 1.0
 */

#include <iostream>
#include <cassert>
#include <cstdint>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>

// 模拟RHI命令队列相关类型
namespace RHI {
    
    // 命令队列类型枚举
    enum class QueueType : uint32_t {
        Graphics = 0,
        Compute = 1,
        Transfer = 2,
        VideoDecode = 3,
        VideoEncode = 4,
        Present = 5,
        Count = 6
    };
    
    // 命令队列优先级
    enum class QueuePriority : uint32_t {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };
    
    // 命令类型枚举
    enum class CommandType : uint32_t {
        Draw = 0,
        Compute = 1,
        Transfer = 2,
        Barrier = 3,
        Present = 4,
        Custom = 5,
        Count = 6
    };
    
    // 命令缓冲区状态
    enum class CommandBufferState : uint32_t {
        Initial = 0,
        Recording = 1,
        Executable = 2,
        Pending = 3,
        Completed = 4,
        Error = 5
    };
    
    // 同步对象类型
    enum class SyncType : uint32_t {
        None = 0,
        Fence = 1,
        Semaphore = 2,
        Event = 3,
        Timeline = 4
    };
    
    // 命令队列描述
    struct QueueDesc {
        QueueType type;
        QueuePriority priority;
        uint32_t index;
        bool canPresent;
        bool timestampSupported;
        
        QueueDesc(QueueType t = QueueType::Graphics, QueuePriority p = QueuePriority::Normal, 
                   uint32_t idx = 0, bool present = false, bool timestamp = false)
            : type(t), priority(p), index(idx), canPresent(present), timestampSupported(timestamp) {}
    };
    
    // 命令缓冲区描述
    struct CommandBufferDesc {
        QueueType queueType;
        bool secondary;
        bool oneTimeSubmit;
        bool simultaneousUse;
        
        CommandBufferDesc(QueueType type = QueueType::Graphics, bool sec = false, 
                          bool oneTime = false, bool simultaneous = false)
            : queueType(type), secondary(sec), oneTimeSubmit(oneTime), simultaneousUse(simultaneous) {}
    };
    
    // 同步对象描述
    struct SyncDesc {
        SyncType type;
        bool signaled;
        uint64_t initialValue;
        
        SyncDesc(SyncType t = SyncType::Fence, bool sig = false, uint64_t init = 0)
            : type(t), signaled(sig), initialValue(init) {}
    };
    
    // 前向声明
    class CommandBuffer;
    class SyncObject;
    
    // 模拟RHI设备类
    class MockDevice {
    private:
        std::atomic<uint64_t> nextCommandBufferId_{1};
        std::atomic<uint64_t> nextSyncObjectId_{1};
        
    public:
        static MockDevice& Instance() {
            static MockDevice instance;
            return instance;
        }
        
        MockDevice() = default;
        ~MockDevice() = default;
        
        // 禁用拷贝构造和赋值
        MockDevice(const MockDevice&) = delete;
        MockDevice& operator=(const MockDevice&) = delete;
        
        uint64_t GenerateCommandBufferId() {
            return nextCommandBufferId_++;
        }
        
        uint64_t GenerateSyncObjectId() {
            return nextSyncObjectId_++;
        }
    };
    
    // 模拟同步对象类
    class SyncObject {
    private:
        MockDevice& device_;
        SyncDesc desc_;
        uint64_t id_;
        std::atomic<bool> signaled_;
        std::atomic<uint64_t> value_;
        
    public:
        SyncObject(const SyncDesc& desc) 
            : device_(MockDevice::Instance()), desc_(desc), 
              id_(device_.GenerateSyncObjectId()), signaled_(desc.signaled), value_(desc.initialValue) {}
        
        ~SyncObject() = default;
        
        bool Signal() {
            signaled_.store(true);
            return true;
        }
        
        bool Reset() {
            signaled_.store(false);
            value_.store(desc_.initialValue);
            return true;
        }
        
        bool IsSignaled() const {
            return signaled_.load();
        }
        
        uint64_t GetValue() const {
            return value_.load();
        }
        
        uint64_t GetId() const {
            return id_;
        }
        
        const SyncDesc& GetDesc() const {
            return desc_;
        }
    };
    
    // 前向声明CommandQueue以便friend
    class CommandQueue;
    
    // 模拟命令缓冲区类
    class CommandBuffer {
    private:
        MockDevice& device_;
        CommandBufferDesc desc_;
        uint64_t id_;
        CommandBufferState state_;
        std::vector<CommandType> recordedCommands_;
        
    public:
        CommandBuffer(const CommandBufferDesc& desc) 
            : device_(MockDevice::Instance()), desc_(desc), 
              id_(device_.GenerateCommandBufferId()), state_(CommandBufferState::Initial) {}
        
        ~CommandBuffer() {
            if (state_ == CommandBufferState::Recording) {
                End();
            }
        }
        
        bool Begin() {
            if (state_ != CommandBufferState::Initial && state_ != CommandBufferState::Completed) {
                return false;
            }
            
            state_ = CommandBufferState::Recording;
            recordedCommands_.clear();
            return true;
        }
        
        bool End() {
            if (state_ != CommandBufferState::Recording) {
                return false;
            }
            
            state_ = CommandBufferState::Executable;
            return true;
        }
        
        bool RecordCommand(CommandType type) {
            if (state_ != CommandBufferState::Recording) {
                return false;
            }
            
            recordedCommands_.push_back(type);
            return true;
        }
        
        bool Reset() {
            state_ = CommandBufferState::Initial;
            recordedCommands_.clear();
            return true;
        }
        
        CommandBufferState GetState() const {
            return state_;
        }
        
        uint64_t GetId() const {
            return id_;
        }
        
        const CommandBufferDesc& GetDesc() const {
            return desc_;
        }
        
        const std::vector<CommandType>& GetRecordedCommands() const {
            return recordedCommands_;
        }
        
        size_t GetCommandCount() const {
            return recordedCommands_.size();
        }
        
    private:
        // 允许CommandQueue访问私有成员
        friend class CommandQueue;
        
        void SetState(CommandBufferState newState) {
            state_ = newState;
        }
    };
    
    // 模拟RHI命令队列类
    class CommandQueue {
    private:
        MockDevice& device_;
        QueueDesc desc_;
        std::vector<CommandBuffer*> activeCommandBuffers_;
        std::atomic<uint64_t> nextSubmitId_{1};
        uint64_t timestampFrequency_;
        
    public:
        CommandQueue(const QueueDesc& desc) 
            : device_(MockDevice::Instance()), desc_(desc), timestampFrequency_(1000000000ULL) {}
        
        ~CommandQueue() {
            for (auto* cmdBuffer : activeCommandBuffers_) {
                delete cmdBuffer;
            }
        }
        
        bool Initialize() {
            return true;
        }
        
        void Shutdown() {
            for (auto* cmdBuffer : activeCommandBuffers_) {
                delete cmdBuffer;
            }
            activeCommandBuffers_.clear();
        }
        
        CommandBuffer* AllocateCommandBuffer(const CommandBufferDesc& desc) {
            auto* cmdBuffer = new CommandBuffer(desc);
            activeCommandBuffers_.push_back(cmdBuffer);
            return cmdBuffer;
        }
        
        void FreeCommandBuffer(CommandBuffer* cmdBuffer) {
            auto it = std::find(activeCommandBuffers_.begin(), activeCommandBuffers_.end(), cmdBuffer);
            if (it != activeCommandBuffers_.end()) {
                activeCommandBuffers_.erase(it);
                delete cmdBuffer;
            }
        }
        
        uint64_t Submit(CommandBuffer* cmdBuffer, SyncObject* waitSync = nullptr, SyncObject* signalSync = nullptr) {
            if (!cmdBuffer || cmdBuffer->GetState() != CommandBufferState::Executable) {
                return 0;
            }
            
            uint64_t submitId = nextSubmitId_++;
            
            // 模拟等待同步对象
            if (waitSync && !waitSync->IsSignaled()) {
                // 模拟等待
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            // 模拟命令执行
            cmdBuffer->SetState(CommandBufferState::Pending);
            
            // 模拟命令执行时间
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            
            cmdBuffer->SetState(CommandBufferState::Completed);
            
            // 模拟发出信号
            if (signalSync) {
                signalSync->Signal();
            }
            
            return submitId;
        }
        
        uint64_t SubmitBatch(const std::vector<CommandBuffer*>& cmdBuffers, 
                            const std::vector<SyncObject*>& waitSyncs = {},
                            const std::vector<SyncObject*>& signalSyncs = {}) {
            if (cmdBuffers.empty()) {
                return 0;
            }
            
            uint64_t submitId = nextSubmitId_++;
            
            // 模拟等待所有同步对象
            for (auto* sync : waitSyncs) {
                if (sync && !sync->IsSignaled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            
            // 模拟批量执行
            for (auto* cmdBuffer : cmdBuffers) {
                if (cmdBuffer && cmdBuffer->GetState() == CommandBufferState::Executable) {
                    cmdBuffer->SetState(CommandBufferState::Pending);
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    cmdBuffer->SetState(CommandBufferState::Completed);
                }
            }
            
            // 模拟发出所有信号
            for (auto* sync : signalSyncs) {
                if (sync) {
                    sync->Signal();
                }
            }
            
            return submitId;
        }
        
        bool WaitIdle() {
            // 模拟等待队列空闲
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return true;
        }
        
        bool GetTimestamp(uint64_t& timestamp) {
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = now.time_since_epoch();
            timestamp = static_cast<uint64_t>(duration.count());
            return true;
        }
        
        bool CalibrateTimestamps(uint64_t& timestamp, uint64_t& cpuTimestamp) {
            GetTimestamp(timestamp);
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = now.time_since_epoch();
            cpuTimestamp = static_cast<uint64_t>(duration.count());
            return true;
        }
        
        uint64_t GetTimestampFrequency() const {
            return timestampFrequency_;
        }
        
        bool Present() {
            if (!desc_.canPresent) {
                return false;
            }
            
            // 模拟呈现
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
            return true;
        }
        
        const QueueDesc& GetDesc() const {
            return desc_;
        }
        
        size_t GetActiveCommandBufferCount() const {
            return activeCommandBuffers_.size();
        }
        
        uint64_t GetNextSubmitId() const {
            return nextSubmitId_.load();
        }
    };
    
} // namespace RHI

// 测试框架宏
#define TEST_ASSERT_EQ(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            std::cerr << "Assert failed at line " << __LINE__ << ": expected " << (expected) << ", got " << (actual) << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_ASSERT_NE(expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            std::cerr << "Assert failed at line " << __LINE__ << ": expected not equal to " << (expected) << ", got " << (actual) << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "Assert failed at line " << __LINE__ << ": expected true, got false" << std::endl; \
            return false; \
        } \
    } while(0)

#define TEST_ASSERT_FALSE(condition) \
    do { \
        if (condition) { \
            std::cerr << "Assert failed at line " << __LINE__ << ": expected false, got true" << std::endl; \
            return false; \
        } \
    } while(0)

// 测试函数声明
bool TestQueueDescription();
bool TestQueueCreation();
bool TestCommandBufferAllocation();
bool TestCommandBufferRecording();
bool TestCommandSubmission();
bool TestBatchSubmission();
bool TestSynchronization();
bool TestTimestamp();
bool TestPresent();
bool TestQueueShutdown();
bool TestMultipleQueues();
bool TestCommandBufferReuse();

// 测试函数实现
bool TestQueueDescription() {
    RHI::QueueDesc desc(RHI::QueueType::Graphics, RHI::QueuePriority::High, 0, true, true);
    
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::QueueType::Graphics), static_cast<uint32_t>(desc.type));
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::QueuePriority::High), static_cast<uint32_t>(desc.priority));
    TEST_ASSERT_EQ(0U, desc.index);
    TEST_ASSERT_TRUE(desc.canPresent);
    TEST_ASSERT_TRUE(desc.timestampSupported);
    
    return true;
}

bool TestQueueCreation() {
    RHI::QueueDesc desc(RHI::QueueType::Graphics, RHI::QueuePriority::Normal);
    RHI::CommandQueue queue(desc);
    
    TEST_ASSERT_TRUE(queue.Initialize());
    TEST_ASSERT_EQ(0U, queue.GetActiveCommandBufferCount());
    TEST_ASSERT_EQ(1ULL, queue.GetNextSubmitId());
    
    const RHI::QueueDesc& queueDesc = queue.GetDesc();
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::QueueType::Graphics), static_cast<uint32_t>(queueDesc.type));
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::QueuePriority::Normal), static_cast<uint32_t>(queueDesc.priority));
    
    queue.Shutdown();
    return true;
}

bool TestCommandBufferAllocation() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
    RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
    
    TEST_ASSERT_TRUE(cmdBuffer != nullptr);
    TEST_ASSERT_EQ(1U, queue.GetActiveCommandBufferCount());
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Initial), 
                   static_cast<uint32_t>(cmdBuffer->GetState()));
    
    queue.FreeCommandBuffer(cmdBuffer);
    TEST_ASSERT_EQ(0U, queue.GetActiveCommandBufferCount());
    
    queue.Shutdown();
    return true;
}

bool TestCommandBufferRecording() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
    RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
    
    // 测试开始录制
    TEST_ASSERT_TRUE(cmdBuffer->Begin());
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Recording), 
                   static_cast<uint32_t>(cmdBuffer->GetState()));
    
    // 测试录制命令
    TEST_ASSERT_TRUE(cmdBuffer->RecordCommand(RHI::CommandType::Draw));
    TEST_ASSERT_TRUE(cmdBuffer->RecordCommand(RHI::CommandType::Barrier));
    TEST_ASSERT_TRUE(cmdBuffer->RecordCommand(RHI::CommandType::Draw));
    TEST_ASSERT_EQ(3U, cmdBuffer->GetCommandCount());
    
    // 测试结束录制
    TEST_ASSERT_TRUE(cmdBuffer->End());
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Executable), 
                   static_cast<uint32_t>(cmdBuffer->GetState()));
    
    // 测试重置
    TEST_ASSERT_TRUE(cmdBuffer->Reset());
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Initial), 
                   static_cast<uint32_t>(cmdBuffer->GetState()));
    TEST_ASSERT_EQ(0U, cmdBuffer->GetCommandCount());
    
    queue.FreeCommandBuffer(cmdBuffer);
    queue.Shutdown();
    return true;
}

bool TestCommandSubmission() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
    RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
    
    // 录制命令
    cmdBuffer->Begin();
    cmdBuffer->RecordCommand(RHI::CommandType::Draw);
    cmdBuffer->End();
    
    // 提交命令
    uint64_t submitId = queue.Submit(cmdBuffer);
    TEST_ASSERT_NE(0ULL, submitId);
    TEST_ASSERT_EQ(1ULL, submitId);
    TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Completed), 
                   static_cast<uint32_t>(cmdBuffer->GetState()));
    
    queue.FreeCommandBuffer(cmdBuffer);
    queue.Shutdown();
    return true;
}

bool TestBatchSubmission() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    // 创建多个命令缓冲区
    std::vector<RHI::CommandBuffer*> cmdBuffers;
    for (int i = 0; i < 3; ++i) {
        RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
        RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
        
        cmdBuffer->Begin();
        cmdBuffer->RecordCommand(RHI::CommandType::Draw);
        cmdBuffer->RecordCommand(RHI::CommandType::Barrier);
        cmdBuffer->End();
        
        cmdBuffers.push_back(cmdBuffer);
    }
    
    // 批量提交
    uint64_t submitId = queue.SubmitBatch(cmdBuffers);
    TEST_ASSERT_NE(0ULL, submitId);
    TEST_ASSERT_EQ(1ULL, submitId);
    
    // 验证所有命令缓冲区都已完成
    for (auto* cmdBuffer : cmdBuffers) {
        TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Completed), 
                       static_cast<uint32_t>(cmdBuffer->GetState()));
        queue.FreeCommandBuffer(cmdBuffer);
    }
    
    queue.Shutdown();
    return true;
}

bool TestSynchronization() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    // 创建同步对象
    RHI::SyncDesc waitDesc(RHI::SyncType::Fence, false);
    RHI::SyncDesc signalDesc(RHI::SyncType::Fence, false);
    RHI::SyncObject* waitSync = new RHI::SyncObject(waitDesc);
    RHI::SyncObject* signalSync = new RHI::SyncObject(signalDesc);
    
    TEST_ASSERT_FALSE(waitSync->IsSignaled());
    TEST_ASSERT_FALSE(signalSync->IsSignaled());
    
    // 发出等待信号
    waitSync->Signal();
    TEST_ASSERT_TRUE(waitSync->IsSignaled());
    
    // 创建命令缓冲区
    RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
    RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
    
    cmdBuffer->Begin();
    cmdBuffer->RecordCommand(RHI::CommandType::Draw);
    cmdBuffer->End();
    
    // 带同步的提交
    uint64_t submitId = queue.Submit(cmdBuffer, waitSync, signalSync);
    TEST_ASSERT_NE(0ULL, submitId);
    TEST_ASSERT_TRUE(signalSync->IsSignaled());
    
    // 重置同步对象
    signalSync->Reset();
    TEST_ASSERT_FALSE(signalSync->IsSignaled());
    
    // 清理
    queue.FreeCommandBuffer(cmdBuffer);
    delete waitSync;
    delete signalSync;
    queue.Shutdown();
    return true;
}

bool TestTimestamp() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics, RHI::QueuePriority::Normal, 0, false, true);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    // 测试时间戳
    uint64_t timestamp;
    TEST_ASSERT_TRUE(queue.GetTimestamp(timestamp));
    TEST_ASSERT_NE(0ULL, timestamp);
    
    // 测试时间戳校准
    uint64_t gpuTimestamp, cpuTimestamp;
    TEST_ASSERT_TRUE(queue.CalibrateTimestamps(gpuTimestamp, cpuTimestamp));
    TEST_ASSERT_NE(0ULL, gpuTimestamp);
    TEST_ASSERT_NE(0ULL, cpuTimestamp);
    
    // 测试时间戳频率
    uint64_t frequency = queue.GetTimestampFrequency();
    TEST_ASSERT_NE(0ULL, frequency);
    TEST_ASSERT_EQ(1000000000ULL, frequency);
    
    queue.Shutdown();
    return true;
}

bool TestPresent() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Present, RHI::QueuePriority::High, 0, true);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    // 测试呈现
    TEST_ASSERT_TRUE(queue.Present());
    
    queue.Shutdown();
    return true;
}

bool TestQueueShutdown() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    // 分配一些命令缓冲区
    std::vector<RHI::CommandBuffer*> cmdBuffers;
    for (int i = 0; i < 5; ++i) {
        RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics);
        cmdBuffers.push_back(queue.AllocateCommandBuffer(cmdDesc));
    }
    
    TEST_ASSERT_EQ(5U, queue.GetActiveCommandBufferCount());
    
    // 关闭队列（应该自动清理资源）
    queue.Shutdown();
    TEST_ASSERT_EQ(0U, queue.GetActiveCommandBufferCount());
    
    return true;
}

bool TestMultipleQueues() {
    std::vector<RHI::CommandQueue*> queues;
    
    // 创建不同类型的队列
    RHI::QueueDesc graphicsDesc(RHI::QueueType::Graphics, RHI::QueuePriority::High);
    RHI::QueueDesc computeDesc(RHI::QueueType::Compute, RHI::QueuePriority::Normal);
    RHI::QueueDesc transferDesc(RHI::QueueType::Transfer, RHI::QueuePriority::Low);
    
    RHI::CommandQueue* graphicsQueue = new RHI::CommandQueue(graphicsDesc);
    RHI::CommandQueue* computeQueue = new RHI::CommandQueue(computeDesc);
    RHI::CommandQueue* transferQueue = new RHI::CommandQueue(transferDesc);
    
    queues.push_back(graphicsQueue);
    queues.push_back(computeQueue);
    queues.push_back(transferQueue);
    
    // 初始化所有队列
    for (auto* queue : queues) {
        TEST_ASSERT_TRUE(queue->Initialize());
    }
    
    // 在每个队列上创建命令缓冲区
    for (auto* queue : queues) {
        RHI::CommandBufferDesc cmdDesc(queue->GetDesc().type);
        RHI::CommandBuffer* cmdBuffer = queue->AllocateCommandBuffer(cmdDesc);
        TEST_ASSERT_TRUE(cmdBuffer != nullptr);
        
        cmdBuffer->Begin();
        cmdBuffer->RecordCommand(RHI::CommandType::Draw);
        cmdBuffer->End();
        
        uint64_t submitId = queue->Submit(cmdBuffer);
        TEST_ASSERT_NE(0ULL, submitId);
        
        queue->FreeCommandBuffer(cmdBuffer);
    }
    
    // 清理
    for (auto* queue : queues) {
        queue->Shutdown();
        delete queue;
    }
    
    return true;
}

bool TestCommandBufferReuse() {
    RHI::QueueDesc queueDesc(RHI::QueueType::Graphics);
    RHI::CommandQueue queue(queueDesc);
    queue.Initialize();
    
    RHI::CommandBufferDesc cmdDesc(RHI::QueueType::Graphics, false, false, true);
    RHI::CommandBuffer* cmdBuffer = queue.AllocateCommandBuffer(cmdDesc);
    
    // 多次使用同一个命令缓冲区
    for (int i = 0; i < 3; ++i) {
        cmdBuffer->Begin();
        cmdBuffer->RecordCommand(RHI::CommandType::Draw);
        cmdBuffer->End();
        
        uint64_t submitId = queue.Submit(cmdBuffer);
        TEST_ASSERT_NE(0ULL, submitId);
        TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Completed), 
                       static_cast<uint32_t>(cmdBuffer->GetState()));
        
        // 重置以便重用
        cmdBuffer->Reset();
        TEST_ASSERT_EQ(static_cast<uint32_t>(RHI::CommandBufferState::Initial), 
                       static_cast<uint32_t>(cmdBuffer->GetState()));
    }
    
    queue.FreeCommandBuffer(cmdBuffer);
    queue.Shutdown();
    return true;
}

// 测试运行函数
bool RunQueueTests() {
    std::cout << "Running RHI Queue Tests..." << std::endl;
    
    struct TestCase {
        const char* name;
        bool (*testFunc)();
    };
    
    TestCase tests[] = {
        {"Queue Description", TestQueueDescription},
        {"Queue Creation", TestQueueCreation},
        {"Command Buffer Allocation", TestCommandBufferAllocation},
        {"Command Buffer Recording", TestCommandBufferRecording},
        {"Command Submission", TestCommandSubmission},
        {"Batch Submission", TestBatchSubmission},
        {"Synchronization", TestSynchronization},
        {"Timestamp", TestTimestamp},
        {"Present", TestPresent},
        {"Queue Shutdown", TestQueueShutdown},
        {"Multiple Queues", TestMultipleQueues},
        {"Command Buffer Reuse", TestCommandBufferReuse}
    };
    
    int totalTests = sizeof(tests) / sizeof(tests[0]);
    int passedTests = 0;
    
    for (int i = 0; i < totalTests; ++i) {
        std::cout << "Running test: " << tests[i].name << " ... ";
        
        if (tests[i].testFunc()) {
            std::cout << "PASSED" << std::endl;
            passedTests++;
        } else {
            std::cout << "FAILED" << std::endl;
        }
    }
    
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Total tests: " << totalTests << std::endl;
    std::cout << "Passed: " << passedTests << std::endl;
    std::cout << "Failed: " << (totalTests - passedTests) << std::endl;
    std::cout << "Success rate: " << (passedTests * 100 / totalTests) << "%" << std::endl;
    
    return passedTests == totalTests;
}

// 主函数
int main() {
    try {
        std::cout << "=== RHI Command Queue Test Suite ===" << std::endl;
        
        bool allTestsPassed = RunQueueTests();
        
        if (allTestsPassed) {
            std::cout << "\nAll tests passed successfully!" << std::endl;
            return 0;
        } else {
            std::cout << "\nSome tests failed!" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Test failed with unknown exception" << std::endl;
        return 1;
    }
}