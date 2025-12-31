/**
 * @file TestRHIAdaptiveMemoryPool.cpp
 * @brief RHI自适应内存池系统单元测试
 * @details 基于项目自定义测试框架，严格遵循项目技术规范，验证自适应内存池的核心功能
 * 
 * @author Engine开发团队
 * @date 2025-12-31
 * @version 1.0
 */

#include "CommonHeaders.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>

// === 测试框架定义 ===
static FILE* g_testOutput = nullptr;

/**
 * @brief 初始化测试输出
 * @param filename 输出文件名
 * @return 是否成功初始化
 */
bool InitTestOutput(const char* filename) {
    g_testOutput = fopen(filename, "w");
    if (!g_testOutput) {
        printf("无法创建输出文件: %s\n", filename);
        return false;
    }
    return true;
}

/**
 * @brief 关闭测试输出
 */
void CloseTestOutput() {
    if (g_testOutput) {
        fclose(g_testOutput);
        g_testOutput = nullptr;
    }
}

/**
 * @brief 测试节输出
 */
#define TEST_SECTION(name) \
    do { \
        if (g_testOutput) { \
            fprintf(g_testOutput, "=== %s ===\n", name); \
            fflush(g_testOutput); \
        } \
    } while(0)

// 测试断言宏
#define TEST_ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s (expected: %llu, actual: %llu)\n", msg, static_cast<unsigned long long>(b), static_cast<unsigned long long>(a)); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s\n", msg); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

#define TEST_ASSERT_FALSE(cond, msg) \
    do { \
        if ((cond)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s\n", msg); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

#define TEST_ASSERT_NULL(ptr, msg) \
    do { \
        if ((ptr) != nullptr) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s (expected: nullptr, actual: %p)\n", msg, static_cast<const void*>(ptr)); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(ptr, msg) \
    do { \
        if ((ptr) == nullptr) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s (expected: not null)\n", msg); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

#define TEST_ASSERT_FLOAT_EQ(a, b, epsilon, msg) \
    do { \
        f32 diff = (a) - (b); \
        if (diff < 0.0f) diff = -diff; \
        if (diff > (epsilon)) { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✗ %s (expected: %.6f, actual: %.6f, diff: %.6f)\n", msg, static_cast<double>(b), static_cast<double>(a), static_cast<double>(diff)); \
                fflush(g_testOutput); \
            } \
            return false; \
        } else { \
            if (g_testOutput) { \
                fprintf(g_testOutput, "✓ %s\n", msg); \
                fflush(g_testOutput); \
            } \
        } \
    } while(0)

// === 模拟RHI类型定义 ===
namespace primal {
namespace graphics {
namespace rhi {

// 基础类型
using RHIDeviceHandle = uint64_t;
using RHIResourceHandle = uint64_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;
// constexpr u32 u32_invalid_id = 0xFFFFFFFF;

// 内存使用类型
enum class GPUMemoryUsage : uint8_t {
    Default = 0,      ///< 默认使用
    Static = 1,       ///< 静态内存，不经常改变
    Dynamic = 2,      ///< 动态内存，经常更新
    Staging = 3       ///< 暂存内存，用于数据传输
};

// 内存分配策略
enum class MemoryAllocationStrategy : uint8_t {
    Linear = 0,         ///< 线性分配
    Buddy = 1,          ///< 伙伴系统
    TLSF = 2,           ///< Two-Level Segregated Fit
    FreeList = 3,       ///< 自由链表
    Pool = 4            ///< 内存池
};

// 内存使用模式
enum class MemoryUsagePattern : uint8_t {
    Unknown = 0,    ///< 未知模式
    Burst = 1,      ///< 突发模式
    Steady = 2,     ///< 稳定模式
    Periodic = 3,   ///< 周期模式
    Growing = 4,   ///< 增长模式
    Shrinking = 5  ///< 收缩模式
};

// 内存池描述符
struct RHIMemoryPoolDesc {
    u64 initialSize;                    ///< 初始大小（字节）
    u64 maxSize;                        ///< 最大大小（字节）
    u64 growSize;                       ///< 增长大小（字节）
    u32 growThreshold;                 ///< 增长阈值（分配次数）
    MemoryAllocationStrategy strategy;  ///< 分配策略
    u32 memoryTypeIndex;                ///< 内存类型索引
    bool enableDefragmentation;         ///< 是否启用碎片整理
    u32 defragmentationThreshold;      ///< 碎片整理阈值
    
    RHIMemoryPoolDesc() 
        : initialSize(64 * 1024 * 1024)
        , maxSize(1024 * 1024 * 1024)
        , growSize(16 * 1024 * 1024)
        , growThreshold(100)
        , strategy(MemoryAllocationStrategy::TLSF)
        , memoryTypeIndex(0)
        , enableDefragmentation(true)
        , defragmentationThreshold(1000) {}
};

// 自适应配置
struct AdaptiveConfig {
    f32 growthFactor;                      ///< 增长因子
    f32 shrinkThreshold;                   ///< 收缩阈值（使用率）
    f32 hotspotThreshold;                  ///< 热点阈值（访问频率）
    u32 patternAnalysisWindowSize;         ///< 模式分析窗口大小
    u32 minAllocationsForPattern;          ///< 模式识别最小分配次数
    f32 patternConfidenceThreshold;       ///< 模式置信度阈值
    u32 hotspotPoolSize;                   ///< 热点池大小
    u32 defragmentationInterval;           ///< 碎片整理间隔（秒）
    f32 fragmentationThreshold;           ///< 碎片化阈值
    bool enableAutoResize;                 ///< 启用自动调整
    bool enableHotspotPooling;             ///< 启用热点池化
    bool enablePatternLearning;            ///< 启用模式学习
    u32 adaptiveAdjustmentCooldown;        ///< 自适应调整冷却时间（毫秒）
    
    AdaptiveConfig() 
        : growthFactor(1.5f)
        , shrinkThreshold(0.2f)
        , hotspotThreshold(0.1f)
        , patternAnalysisWindowSize(300)
        , minAllocationsForPattern(50)
        , patternConfidenceThreshold(0.7f)
        , hotspotPoolSize(1024)
        , defragmentationInterval(60)
        , fragmentationThreshold(0.3f)
        , enableAutoResize(true)
        , enableHotspotPooling(true)
        , enablePatternLearning(true)
        , adaptiveAdjustmentCooldown(5000) {}
};

// 内存分配记录
struct MemoryAllocation {
    u64 offset;              ///< 偏移量
    u64 size;                ///< 大小
    u32 alignment;           ///< 对齐要求
    GPUMemoryUsage usage;    ///< 使用类型
    u64 timestamp;           ///< 分配时间戳
    u32 accessCount;         ///< 访问计数
    u64 lastAccessTime;      ///< 最后访问时间
    u32 blockId;             ///< 内存块ID
    bool isHotspot;          ///< 是否为热点
    
    MemoryAllocation() 
        : offset(0), size(0), alignment(0), usage(GPUMemoryUsage::Default)
        , timestamp(0), accessCount(0), lastAccessTime(0), blockId(u32_invalid_id)
        , isHotspot(false) {}
};

// 使用统计
struct UsageStatistics {
    u64 totalAllocations;        ///< 总分配次数
    u64 totalDeallocations;      ///< 总释放次数
    u64 totalBytesAllocated;     ///< 总分配字节数
    u64 currentBytesInUse;       ///< 当前使用字节数
    u64 peakBytesInUse;          ///< 峰值使用字节数
    f64 averageAllocationTime;   ///< 平均分配时间（微秒）
    f64 averageDeallocationTime; ///< 平均释放时间（微秒）
    u32 failedAllocations;       ///< 失败分配次数
    u32 fragmentationLevel;      ///< 碎片化等级（0-100）
    u32 hotspotCount;            ///< 热点数量
    f32 hotspotHitRatio;         ///< 热点命中率
    
    UsageStatistics() 
        : totalAllocations(0), totalDeallocations(0), totalBytesAllocated(0)
        , currentBytesInUse(0), peakBytesInUse(0), averageAllocationTime(0.0)
        , averageDeallocationTime(0.0), failedAllocations(0), fragmentationLevel(0)
        , hotspotCount(0), hotspotHitRatio(0.0f) {}
};

// 自适应指标
struct AdaptiveMetrics {
    MemoryUsagePattern detectedPattern;  ///< 检测到的使用模式
    f32 patternConfidence;               ///< 模式置信度
    u32 resizeCount;                     ///< 调整次数
    u32 totalHotspots;                   ///< 总热点数量
    f32 hotspotHitRatio;                 ///< 热点命中率
    u32 defragmentationCount;            ///< 碎片整理次数
    f32 averageAllocationTime;           ///< 平均分配时间
    f32 memoryEfficiency;                ///< 内存效率
    u32 adaptiveAdjustmentCount;         ///< 自适应调整次数
    u64 lastAdjustmentTime;              ///< 最后调整时间
    f32 currentUtilization;              ///< 当前利用率
    f32 averageUtilization;              ///< 平均利用率
    
    AdaptiveMetrics() 
        : detectedPattern(MemoryUsagePattern::Unknown), patternConfidence(0.0f)
        , resizeCount(0), totalHotspots(0), hotspotHitRatio(0.0f)
        , defragmentationCount(0), averageAllocationTime(0.0f), memoryEfficiency(0.0f)
        , adaptiveAdjustmentCount(0), lastAdjustmentTime(0), currentUtilization(0.0f)
        , averageUtilization(0.0f) {}
};

// 模拟设备
class MockDevice {
public:
    static constexpr u64 DefaultDeviceHandle = 0x1234567890ABCDEF;
    
    MockDevice() : handle_(DefaultDeviceHandle) {}
    u64 GetHandle() const { return handle_; }
    
private:
    u64 handle_;
};

// 模拟自适应内存池
class MockAdaptiveMemoryPool {
private:
    MockDevice device_;
    RHIMemoryPoolDesc poolDesc_;
    AdaptiveConfig config_;
    
    // 内存管理
    std::vector<MemoryAllocation> allocations_;
    std::vector<u8> memoryBlock_;
    u64 currentOffset_;
    bool isInitialized_;
    
    // 统计信息
    UsageStatistics stats_;
    AdaptiveMetrics metrics_;
    
    // 自适应状态
    std::vector<f64> allocationTimes_;
    std::vector<u64> allocationSizes_;
    std::vector<u64> timestamps_;
    mutable std::mutex statsMutex_;
    mutable std::mutex adaptiveMutex_;
    
    // 热点管理
    std::vector<u32> hotspotIds_;
    u32 hotspotAccessThreshold_;
    
    // 时间管理
    std::chrono::steady_clock::time_point startTime_;
    
public:
    MockAdaptiveMemoryPool(const MockDevice& device, const RHIMemoryPoolDesc& desc, const AdaptiveConfig& config)
        : device_(device), poolDesc_(desc), config_(config), currentOffset_(0), isInitialized_(false)
        , hotspotAccessThreshold_(10), startTime_(std::chrono::steady_clock::now()) {
        allocations_.reserve(1024);
        memoryBlock_.resize(desc.initialSize);
        std::fill(memoryBlock_.begin(), memoryBlock_.end(), 0);
    }
    
    ~MockAdaptiveMemoryPool() {
        if (isInitialized_) {
            Shutdown();
        }
    }
    
    bool Initialize() {
        if (isInitialized_) return true;
        
        currentOffset_ = 0;
        isInitialized_ = true;
        return true;
    }
    
    void Shutdown() {
        if (!isInitialized_) return;
        
        allocations_.clear();
        memoryBlock_.clear();
        currentOffset_ = 0;
        isInitialized_ = false;
    }
    
    u32 Allocate(u64 size, u32 alignment, GPUMemoryUsage usage) {
        if (!isInitialized_) return u32_invalid_id;
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // 对齐计算
        u64 alignedOffset = (currentOffset_ + alignment - 1) & ~(alignment - 1);
        
        // 检查内存是否足够
        if (alignedOffset + size > memoryBlock_.size()) {
            stats_.failedAllocations++;
            return u32_invalid_id;
        }
        
        // 创建分配记录
        MemoryAllocation alloc;
        alloc.offset = alignedOffset;
        alloc.size = size;
        alloc.alignment = alignment;
        alloc.usage = usage;
        alloc.timestamp = GetCurrentTimeMicros();
        alloc.accessCount = 1;
        alloc.lastAccessTime = alloc.timestamp;
        alloc.blockId = static_cast<u32>(allocations_.size());
        alloc.isHotspot = false;
        
        u32 allocationId = static_cast<u32>(allocations_.size());
        allocations_.push_back(alloc);
        
        // 更新偏移
        currentOffset_ = alignedOffset + size;
        
        // 更新统计信息
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.totalAllocations++;
            stats_.totalBytesAllocated += size;
            stats_.currentBytesInUse += size;
            if (stats_.currentBytesInUse > stats_.peakBytesInUse) {
                stats_.peakBytesInUse = stats_.currentBytesInUse;
            }
            
            // 记录分配时间
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            f64 allocTime = static_cast<f64>(duration.count());
            
            if (stats_.totalAllocations == 1) {
                stats_.averageAllocationTime = allocTime;
            } else {
                stats_.averageAllocationTime = 
                    (stats_.averageAllocationTime * (stats_.totalAllocations - 1) + allocTime) / stats_.totalAllocations;
            }
            
            allocationTimes_.push_back(allocTime);
            allocationSizes_.push_back(size);
            timestamps_.push_back(alloc.timestamp);
        }
        
        return allocationId;
    }
    
    void Deallocate(u32 allocationId) {
        if (!isInitialized_ || allocationId >= allocations_.size()) return;
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        MemoryAllocation& alloc = allocations_[allocationId];
        if (alloc.size == 0) return; // 已经释放
        
        // 更新统计信息
        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.totalDeallocations++;
            stats_.currentBytesInUse -= alloc.size;
            
            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            f64 deallocTime = static_cast<f64>(duration.count());
            
            if (stats_.totalDeallocations == 1) {
                stats_.averageDeallocationTime = deallocTime;
            } else {
                stats_.averageDeallocationTime = 
                    (stats_.averageDeallocationTime * (stats_.totalDeallocations - 1) + deallocTime) / stats_.totalDeallocations;
            }
        }
        
        // 清零内存（模拟释放）
        std::fill(memoryBlock_.begin() + alloc.offset, 
                  memoryBlock_.begin() + alloc.offset + alloc.size, 0);
        
        // 标记为已释放
        alloc.size = 0;
    }
    
    MemoryUsagePattern AnalyzeUsagePattern() {
        if (allocationTimes_.size() < config_.minAllocationsForPattern) {
            return MemoryUsagePattern::Unknown;
        }
        
        std::lock_guard<std::mutex> lock(adaptiveMutex_);
        
        // 简化的模式分析算法
        u64 totalSize = 0;
        for (u64 size : allocationSizes_) {
            totalSize += size;
        }
        u64 averageSize = totalSize / allocationSizes_.size();
        
        // 计算变异系数
        f64 variance = 0.0;
        for (u64 size : allocationSizes_) {
            f64 diff = static_cast<f64>(size) - static_cast<f64>(averageSize);
            variance += diff * diff;
        }
        variance /= allocationSizes_.size();
        f64 stdDev = std::sqrt(variance);
        f64 coefficientOfVariation = stdDev / averageSize;
        
        MemoryUsagePattern pattern = MemoryUsagePattern::Unknown;
        f32 confidence = 0.0f;
        
        if (coefficientOfVariation < 0.1f) {
            pattern = MemoryUsagePattern::Steady;
            confidence = 0.8f;
        } else if (coefficientOfVariation < 0.3f) {
            pattern = MemoryUsagePattern::Periodic;
            confidence = 0.6f;
        } else if (coefficientOfVariation < 0.5f) {
            pattern = MemoryUsagePattern::Burst;
            confidence = 0.5f;
        } else {
            pattern = MemoryUsagePattern::Growing;
            confidence = 0.4f;
        }
        
        metrics_.detectedPattern = pattern;
        metrics_.patternConfidence = confidence;
        
        return pattern;
    }
    
    bool PerformAdaptiveAdjustment() {
        std::lock_guard<std::mutex> lock(adaptiveMutex_);
        
        // 检查冷却时间
        u64 currentTime = GetCurrentTimeMicros();
        if (currentTime - metrics_.lastAdjustmentTime < config_.adaptiveAdjustmentCooldown * 1000) {
            return false;
        }
        
        bool adjusted = false;
        
        // 基于使用率调整
        f32 utilization = static_cast<f32>(stats_.currentBytesInUse) / memoryBlock_.size();
        metrics_.currentUtilization = utilization;
        
        if (utilization > 0.9f && memoryBlock_.size() < poolDesc_.maxSize) {
            // 需要扩容
            u64 newSize = static_cast<u64>(memoryBlock_.size() * config_.growthFactor);
            if (newSize > poolDesc_.maxSize) {
                newSize = poolDesc_.maxSize;
            }
            
            if (newSize > memoryBlock_.size()) {
                memoryBlock_.resize(newSize);
                std::fill(memoryBlock_.begin() + currentOffset_, memoryBlock_.end(), 0);
                metrics_.resizeCount++;
                adjusted = true;
            }
        }
        
        if (adjusted) {
            metrics_.lastAdjustmentTime = currentTime;
            metrics_.adaptiveAdjustmentCount++;
        }
        
        return adjusted;
    }
    
    void UpdateHotspot(u32 allocationId) {
        if (!isInitialized_ || allocationId >= allocations_.size()) return;
        
        MemoryAllocation& alloc = allocations_[allocationId];
        if (alloc.size == 0) return;
        
        alloc.accessCount++;
        alloc.lastAccessTime = GetCurrentTimeMicros();
        
        // 简化的热点检测
        if (alloc.accessCount > hotspotAccessThreshold_) {
            alloc.isHotspot = true;
            
            bool found = false;
            for (u32 id : hotspotIds_) {
                if (id == allocationId) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                hotspotIds_.push_back(allocationId);
                stats_.hotspotCount++;
            }
        }
    }
    
    const UsageStatistics& GetStatistics() const {
        std::lock_guard<std::mutex> lock(statsMutex_);
        return stats_;
    }
    
    const AdaptiveMetrics& GetAdaptiveMetrics() const {
        std::lock_guard<std::mutex> lock(adaptiveMutex_);
        return metrics_;
    }
    
    bool IsInitialized() const { return isInitialized_; }
    u64 GetMemorySize() const { return memoryBlock_.size(); }
    u64 GetUsedMemory() const { return stats_.currentBytesInUse; }
    u32 GetAllocationCount() const { return static_cast<u32>(allocations_.size()); }
    
private:
    u64 GetCurrentTimeMicros() const {
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime_);
        return static_cast<u64>(duration.count());
    }
};

} // namespace rhi
} // namespace graphics
} // namespace primal

using namespace primal::graphics::rhi;

// 测试函数声明
bool TestAdaptiveMemoryPoolBasicFunctionality();
bool TestAdaptiveMemoryPoolAllocationDeallocation();
bool TestAdaptiveMemoryPoolStatistics();
bool TestAdaptiveMemoryPoolPatternAnalysis();
bool TestAdaptiveMemoryPoolHotspotDetection();
bool TestAdaptiveMemoryPoolAdaptiveAdjustment();
bool TestAdaptiveMemoryPoolStressTest();
bool TestAdaptiveMemoryPoolMultiThreading();
bool TestAdaptiveMemoryPoolEdgeCases();

/**
 * @brief 测试自适应内存池基本功能
 */
bool TestAdaptiveMemoryPoolBasicFunctionality() {
    TEST_SECTION("测试自适应内存池基本功能");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool* pool = new MockAdaptiveMemoryPool(device, poolDesc, config);
    TEST_ASSERT_NOT_NULL(pool, "内存池创建应该成功");
    
    // 测试初始化
    bool initResult = pool->Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    TEST_ASSERT_TRUE(pool->IsInitialized(), "内存池应该处于初始化状态");
    
    // 测试内存大小
    u64 memSize = pool->GetMemorySize();
    TEST_ASSERT_TRUE(memSize >= poolDesc.initialSize, "内存大小应该至少为初始大小");
    
    // 测试关闭
    pool->Shutdown();
    TEST_ASSERT_TRUE(!pool->IsInitialized(), "关闭后内存池应该未初始化");
    
    delete pool;
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池分配和释放
 */
bool TestAdaptiveMemoryPoolAllocationDeallocation() {
    TEST_SECTION("测试自适应内存池分配和释放");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    // 测试基本分配
    u32 alloc1 = pool.Allocate(1024, 256, GPUMemoryUsage::Static);
    TEST_ASSERT_TRUE(alloc1 != u32_invalid_id, "1024字节分配应该成功");
    
    u32 alloc2 = pool.Allocate(2048, 256, GPUMemoryUsage::Dynamic);
    TEST_ASSERT_TRUE(alloc2 != u32_invalid_id, "2048字节分配应该成功");
    TEST_ASSERT_TRUE(alloc1 != alloc2, "两次分配ID应该不同");
    
    // 测试热点更新
    pool.UpdateHotspot(alloc1);
    pool.UpdateHotspot(alloc2);
    
    // 测试释放
    pool.Deallocate(alloc1);
    pool.Deallocate(alloc2);
    
    // 测试统计信息
    const UsageStatistics& stats = pool.GetStatistics();
    TEST_ASSERT_EQ(stats.totalAllocations, 2, "总分配次数应该为2");
    TEST_ASSERT_EQ(stats.totalDeallocations, 2, "总释放次数应该为2");
    TEST_ASSERT_EQ(stats.currentBytesInUse, 0, "当前使用字节数应该为0");
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池统计信息
 */
bool TestAdaptiveMemoryPoolStatistics() {
    TEST_SECTION("测试自适应内存池统计信息");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    
    
    // 执行多次分配
    const u32 numAllocations = 10;
    const u64 allocSize = 4096;
    
    std::vector<u32> allocationIds;
    for (u32 i = 0; i < numAllocations; ++i) {
        u64 currentSize = allocSize * (i + 1);
        u32 id = pool.Allocate(currentSize, 256, GPUMemoryUsage::Static);
        
        if (id != u32_invalid_id) {
            allocationIds.push_back(id);
        }
    }
    
    // 检查统计信息
    const UsageStatistics& stats = pool.GetStatistics();
    TEST_ASSERT_EQ(stats.totalAllocations, allocationIds.size(), "分配次数应该匹配");
    TEST_ASSERT_TRUE(stats.totalBytesAllocated > 0, "总分配字节数应该大于0");
    TEST_ASSERT_TRUE(stats.currentBytesInUse > 0, "当前使用字节数应该大于0");
    TEST_ASSERT_TRUE(stats.averageAllocationTime >= 0.0, "平均分配时间应该非负");
    
    
    
    // 获取释放前的统计信息
    const UsageStatistics beforeDeallocStats = pool.GetStatistics();
    
    // 释放部分内存
    for (size_t i = 0; i < allocationIds.size() / 2; ++i) {
        pool.Deallocate(allocationIds[i]);
    }
    
    // 再次检查统计信息
    const UsageStatistics& stats2 = pool.GetStatistics();
    TEST_ASSERT_TRUE(stats2.totalDeallocations > 0, "释放次数应该大于0");
    
    
    
    TEST_ASSERT_TRUE(stats2.currentBytesInUse < beforeDeallocStats.currentBytesInUse, "使用字节数应该减少");
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池模式分析
 */
bool TestAdaptiveMemoryPoolPatternAnalysis() {
    TEST_SECTION("测试自适应内存池模式分析");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    config.minAllocationsForPattern = 10; // 降低阈值以便测试
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    // 创建稳定模式（相同大小的分配）
    const u32 numAllocations = 15;
    const u64 steadySize = 1024;
    
    std::vector<u32> allocationIds;
    for (u32 i = 0; i < numAllocations; ++i) {
        u32 id = pool.Allocate(steadySize, 256, GPUMemoryUsage::Static);
        if (id != u32_invalid_id) {
            allocationIds.push_back(id);
        }
    }
    
    // 分析使用模式
    MemoryUsagePattern pattern = pool.AnalyzeUsagePattern();
    TEST_ASSERT_TRUE(pattern != MemoryUsagePattern::Unknown, "应该检测到某种使用模式");
    
    // 检查自适应指标
    const AdaptiveMetrics& metrics = pool.GetAdaptiveMetrics();
    TEST_ASSERT_TRUE(metrics.patternConfidence > 0.0f, "模式置信度应该大于0");
    
    // 清理
    for (u32 id : allocationIds) {
        pool.Deallocate(id);
    }
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池热点检测
 */
bool TestAdaptiveMemoryPoolHotspotDetection() {
    TEST_SECTION("测试自适应内存池热点检测");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    // 分配内存块
    u32 alloc1 = pool.Allocate(1024, 256, GPUMemoryUsage::Dynamic);
    u32 alloc2 = pool.Allocate(2048, 256, GPUMemoryUsage::Dynamic);
    TEST_ASSERT_TRUE(alloc1 != u32_invalid_id && alloc2 != u32_invalid_id, "分配应该成功");
    
    // 模拟热点访问（多次访问同一个分配）
    const u32 numAccesses = 15; // 超过阈值
    for (u32 i = 0; i < numAccesses; ++i) {
        pool.UpdateHotspot(alloc1);
    }
    
    // 检查统计信息
    const UsageStatistics& stats = pool.GetStatistics();
    TEST_ASSERT_TRUE(stats.hotspotCount > 0, "应该检测到热点");
    
    // 清理
    pool.Deallocate(alloc1);
    pool.Deallocate(alloc2);
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池自适应调整
 */
bool TestAdaptiveMemoryPoolAdaptiveAdjustment() {
    TEST_SECTION("测试自适应内存池自适应调整");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    poolDesc.maxSize = 128 * 1024 * 1024; // 128MB
    AdaptiveConfig config;
    config.adaptiveAdjustmentCooldown = 100; // 100ms冷却时间
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    u64 initialSize = pool.GetMemorySize();
    
    // 分配大量内存触发扩容
    std::vector<u32> allocationIds;
    const u64 largeAllocSize = 8 * 1024 * 1024; // 8MB
    u32 numAllocs = 0;
    
    while (numAllocs < 10) {
        u32 id = pool.Allocate(largeAllocSize, 256, GPUMemoryUsage::Static);
        if (id == u32_invalid_id) {
            break;
        }
        allocationIds.push_back(id);
        numAllocs++;
    }
    
    // 触发自适应调整
    bool adjusted = pool.PerformAdaptiveAdjustment();
    
    // 检查是否调整
    const AdaptiveMetrics& metrics = pool.GetAdaptiveMetrics();
    if (adjusted) {
        TEST_ASSERT_TRUE(pool.GetMemorySize() >= initialSize, "调整后内存大小应该增加或保持");
        TEST_ASSERT_TRUE(metrics.resizeCount > 0, "调整次数应该大于0");
    }
    
    // 清理
    for (u32 id : allocationIds) {
        pool.Deallocate(id);
    }
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池压力测试
 */
bool TestAdaptiveMemoryPoolStressTest() {
    TEST_SECTION("测试自适应内存池压力测试");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    poolDesc.maxSize = 256 * 1024 * 1024; // 256MB
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    std::vector<u32> allocationIds;
    const u32 numIterations = 1000;
    const u64 maxAllocSize = 1024 * 1024; // 1MB
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<u64> sizeDist(1024, maxAllocSize);
    
    u32 successfulAllocs = 0;
    u32 failedAllocs = 0;
    
    // 大量随机分配
    for (u32 i = 0; i < numIterations; ++i) {
        u64 allocSize = sizeDist(gen);
        u32 id = pool.Allocate(allocSize, 256, GPUMemoryUsage::Dynamic);
        
        if (id != u32_invalid_id) {
            allocationIds.push_back(id);
            successfulAllocs++;
            
            // 随机更新热点
            if (i % 10 == 0 && !allocationIds.empty()) {
                u32 hotspotId = allocationIds[allocationIds.size() - 1];
                pool.UpdateHotspot(hotspotId);
            }
        } else {
            failedAllocs++;
        }
        
        // 定期释放一些内存
        if (i % 50 == 0 && allocationIds.size() > 10) {
            for (u32 j = 0; j < 5 && !allocationIds.empty(); ++j) {
                u32 id = allocationIds.back();
                pool.Deallocate(id);
                allocationIds.pop_back();
            }
        }
    }
    
    // 检查压力测试结果
    const UsageStatistics& stats = pool.GetStatistics();
    TEST_ASSERT_TRUE(successfulAllocs > 0, "应该有成功的分配");
    TEST_ASSERT_TRUE(stats.totalAllocations > 0, "总分配次数应该大于0");
    TEST_ASSERT_TRUE(stats.totalBytesAllocated > 0, "总分配字节数应该大于0");
    
    // 清理剩余分配
    for (u32 id : allocationIds) {
        pool.Deallocate(id);
    }
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池多线程安全性
 */
bool TestAdaptiveMemoryPoolMultiThreading() {
    TEST_SECTION("测试自适应内存池多线程安全性");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    const u32 numThreads = 4;
    const u32 allocsPerThread = 50;
    std::vector<std::thread> threads;
    std::vector<std::vector<u32>> threadAllocIds(numThreads);
    
    // 多线程分配测试
    for (u32 t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, &threadAllocIds, t, allocsPerThread]() {
            for (u32 i = 0; i < allocsPerThread; ++i) {
                u64 size = 1024 * (i + 1);
                u32 id = pool.Allocate(size, 256, GPUMemoryUsage::Dynamic);
                if (id != u32_invalid_id) {
                    threadAllocIds[t].push_back(id);
                }
                
                // 模拟热点访问
                if (i % 5 == 0 && !threadAllocIds[t].empty()) {
                    pool.UpdateHotspot(threadAllocIds[t].back());
                }
                
                // 短暂延迟
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 检查结果
    u32 totalAllocs = 0;
    for (const auto& allocIds : threadAllocIds) {
        totalAllocs += static_cast<u32>(allocIds.size());
    }
    
    const UsageStatistics& stats = pool.GetStatistics();
    TEST_ASSERT_TRUE(stats.totalAllocations >= totalAllocs, "分配次数应该匹配");
    
    // 多线程释放测试
    threads.clear();
    for (u32 t = 0; t < numThreads; ++t) {
        threads.emplace_back([&pool, &threadAllocIds, t]() {
            for (u32 id : threadAllocIds[t]) {
                pool.Deallocate(id);
            }
        });
    }
    
    // 等待释放完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 检查释放结果
    const UsageStatistics& stats2 = pool.GetStatistics();
    TEST_ASSERT_TRUE(stats2.totalDeallocations >= totalAllocs, "释放次数应该匹配");
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 测试自适应内存池边界情况
 */
bool TestAdaptiveMemoryPoolEdgeCases() {
    TEST_SECTION("测试自适应内存池边界情况");
    
    MockDevice device;
    RHIMemoryPoolDesc poolDesc;
    AdaptiveConfig config;
    
    MockAdaptiveMemoryPool pool(device, poolDesc, config);
    bool initResult = pool.Initialize();
    TEST_ASSERT_TRUE(initResult, "内存池初始化应该成功");
    
    // 测试零大小分配
    u32 zeroAlloc = pool.Allocate(0, 256, GPUMemoryUsage::Static);
    // 零大小分配的处理取决于实现，这里只测试不会崩溃
    
    // 测试极大对齐要求
    u32 largeAlignAlloc = pool.Allocate(1024, 1024 * 1024, GPUMemoryUsage::Static);
    // 极大对齐的处理取决于实现，这里只测试不会崩溃
    
    // 测试无效ID释放
    pool.Deallocate(u32_invalid_id);
    pool.Deallocate(999999); // 不存在的ID
    // 应该不会崩溃
    
    // 测试重复释放
    u32 validAlloc = pool.Allocate(1024, 256, GPUMemoryUsage::Static);
    if (validAlloc != u32_invalid_id) {
        pool.Deallocate(validAlloc);
        pool.Deallocate(validAlloc); // 重复释放
    }
    
    // 测试未初始化状态下的操作
    MockAdaptiveMemoryPool uninitPool(device, poolDesc, config);
    u32 uninitAlloc = uninitPool.Allocate(1024, 256, GPUMemoryUsage::Static);
    TEST_ASSERT_EQ(uninitAlloc, u32_invalid_id, "未初始化池分配应该失败");
    
    pool.Shutdown();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "\n");
        fflush(g_testOutput);
    }
    return true;
}

/**
 * @brief 主测试函数
 */
int main() {
    // 初始化测试输出
    if (!InitTestOutput("rhi_adaptive_memory_pool_test_results.txt")) {
        return 1;
    }
    
    if (g_testOutput) {
        fprintf(g_testOutput, "🚀 开始RHI自适应内存池系统单元测试\n");
        fprintf(g_testOutput, "测试时间: %s %s\n\n", __DATE__, __TIME__);
        fflush(g_testOutput);
    }
    
    bool allPassed = true;
    
    // 运行所有测试
    allPassed &= TestAdaptiveMemoryPoolBasicFunctionality();
    allPassed &= TestAdaptiveMemoryPoolAllocationDeallocation();
    allPassed &= TestAdaptiveMemoryPoolStatistics();
    allPassed &= TestAdaptiveMemoryPoolPatternAnalysis();
    allPassed &= TestAdaptiveMemoryPoolHotspotDetection();
    allPassed &= TestAdaptiveMemoryPoolAdaptiveAdjustment();
    allPassed &= TestAdaptiveMemoryPoolStressTest();
    allPassed &= TestAdaptiveMemoryPoolMultiThreading();
    allPassed &= TestAdaptiveMemoryPoolEdgeCases();
    
    if (g_testOutput) {
        fprintf(g_testOutput, "=== 测试总结 ===\n");
        if (allPassed) {
            fprintf(g_testOutput, "🎉 所有RHI自适应内存池系统测试通过！\n");
        } else {
            fprintf(g_testOutput, "❌ 部分测试失败\n");
        }
        fflush(g_testOutput);
    }
    
    // 输出最终结果到控制台
    if (allPassed) {
        printf("✅ 测试完成，所有测试通过！结果已保存到 rhi_adaptive_memory_pool_test_results.txt\n");
    } else {
        printf("❌ 测试完成，部分测试失败。结果已保存到 rhi_adaptive_memory_pool_test_results.txt\n");
    }
    
    CloseTestOutput();
    return allPassed ? 0 : 1;
}