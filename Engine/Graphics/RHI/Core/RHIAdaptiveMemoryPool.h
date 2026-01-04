/**
 * @file RHIAdaptiveMemoryPool.h
 * @brief RHI自适应内存池系统
 * @details 提供智能内存分配、自适应大小调整、使用模式学习和性能优化功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"

#include "RHIMemoryPool.h"

namespace primal::graphics::rhi {

// === 前向声明 ===

class RHIDeviceBaseBase;
struct AdaptiveConfig;
struct UsagePatternData;    
struct HotspotInfo;
struct MemoryAllocationRecord;

/**
 * @brief 内存使用模式枚举
 * @details 描述不同的内存使用模式特征
 */
enum class MemoryUsagePattern : uint8_t {
    Unknown = 0,        ///< 未知模式
    Burst = 1,          ///< 突发模式：短时间内大量分配
    Steady = 2,         ///< 稳定模式：持续的稳定分配
    Periodic = 3,       ///< 周期模式：周期性的使用高峰
    Growing = 4,        ///< 增长模式：使用量逐渐增长
    Shrinking = 5       ///< 收缩模式：使用量逐渐减少
};

/**
 * @brief 内存块热点级别枚举
 */
enum class MemoryHotspotLevel : uint8_t {
    Cold = 0,           ///< 冷点：很少使用
    Warm = 1,           ///< 温点：偶尔使用
    Hot = 2,            ///< 热点：频繁使用
    Critical = 3        ///< 关键：极高频率使用
};

/**
 * @brief 内存分配记录
 * @details 记录每次内存分配的详细信息
 */
struct AllocationRecord {
    u64 timestamp;              ///< 分配时间戳（微秒）
    u64 deallocationTimestamp;  ///< 释放时间戳（微秒，0表示未释放）
    u32 size;                   ///< 分配大小
    u32 alignment;              ///< 对齐要求
    GPUMemoryUsage usage;       ///< 内存用途
    u32 accessCount;            ///< 访问次数
    u64 totalAccessTime;        ///< 总访问时间（微秒）
    MemoryHotspotLevel hotspotLevel; ///< 热点级别
    
    AllocationRecord() : timestamp(0), deallocationTimestamp(0), size(0), 
                        alignment(0), usage(GPUMemoryUsage::Unknown), accessCount(0),
                        totalAccessTime(0), hotspotLevel(MemoryHotspotLevel::Cold) {}
};

/**
 * @brief 内存使用统计窗口
 * @details 用于分析内存使用模式的时间窗口
 */
struct UsageWindow {
    u64 startTime;               ///< 窗口开始时间
    u64 endTime;                 ///< 窗口结束时间
    u32 allocationCount;         ///< 分配次数
    u32 deallocationCount;       ///< 释放次数
    u64 peakUsage;               ///< 峰值使用量
    u64 averageUsage;            ///< 平均使用量
    f32 fragmentationRatio;      ///< 碎片化比例
    u32 hotspotCount;            ///< 热点内存块数量
    
    UsageWindow() : startTime(0), endTime(0), allocationCount(0), 
                   deallocationCount(0), peakUsage(0), averageUsage(0),
                   fragmentationRatio(0.0f), hotspotCount(0) {}
};

/**
 * @brief 自适应配置参数
 */
struct AdaptiveConfig {
    // 模式学习参数
    u32 patternAnalysisWindowSize;    ///< 模式分析窗口大小（秒）
    u32 patternDetectionThreshold;    ///< 模式检测阈值
    f32 patternConfidenceThreshold;   ///< 模式置信度阈值
    
    // 自适应调整参数
    f32 growthFactor;                 ///< 增长因子
    f32 shrinkFactor;                 ///< 收缩因子
    u32 minPoolSize;                  ///< 最小池大小
    u32 maxPoolSize;                  ///< 最大池大小
    u32 resizeCooldown;               ///< 调整冷却时间（秒）
    
    // 热点检测参数
    u32 hotspotAccessThreshold;       ///< 热点访问阈值
    u32 hotspotTimeWindow;            ///< 热点检测时间窗口（秒）
    f32 hotspotMemoryRatio;           ///< 热点内存比例阈值
    
    // 碎片整理参数
    f32 defragmentationThreshold;     ///< 碎片整理阈值
    u32 defragmentationInterval;       ///< 碎片整理间隔（秒）
    
    AdaptiveConfig() 
        : patternAnalysisWindowSize(300), patternDetectionThreshold(10), 
          patternConfidenceThreshold(0.8f), growthFactor(1.5f), shrinkFactor(0.8f),
          minPoolSize(16 * 1024 * 1024), maxPoolSize(1024 * 1024 * 1024),
          resizeCooldown(60), hotspotAccessThreshold(100), hotspotTimeWindow(60),
          hotspotMemoryRatio(0.3f), defragmentationThreshold(0.4f), 
          defragmentationInterval(120) {}
};

/**
 * @brief 自适应内存池性能指标
 */
struct AdaptiveMetrics {
    // 模式识别指标
    MemoryUsagePattern detectedPattern; ///< 检测到的使用模式
    f32 patternConfidence;              ///< 模式置信度
    u64 patternStartTime;               ///< 模式开始时间
    
    // 自适应调整指标
    u32 resizeCount;                    ///< 调整次数
    u64 totalResizeTime;                ///< 总调整时间
    u64 lastResizeTime;                 ///< 上次调整时间
    
    // 热点统计指标
    u32 totalHotspots;                  ///< 总热点数量
    u64 hotspotMemorySize;             ///< 热点内存总大小
    f32 hotspotHitRatio;                ///< 热点命中率
    
    // 性能优化指标
    u32 defragmentationCount;           ///< 碎片整理次数
    u64 totalDefragmentationTime;       ///< 总碎片整理时间
    f32 averageAllocationTime;          ///< 平均分配时间
    f32 averageDeallocationTime;        ///< 平均释放时间
    
    // 内存效率指标
    f32 memoryEfficiency;               ///< 内存效率
    f32 allocationSuccessRate;          ///< 分配成功率
    u32 failedAllocationCount;          ///< 失败分配次数
    u32 allocationCount;                ///< 总分配次数
    u32 deallocationCount;              ///< 总释放次数
    
    AdaptiveMetrics() : detectedPattern(MemoryUsagePattern::Unknown), 
                       patternConfidence(0.0f), patternStartTime(0), 
                       resizeCount(0), totalResizeTime(0), lastResizeTime(0),
                       totalHotspots(0), hotspotMemorySize(0), hotspotHitRatio(0.0f),
                       defragmentationCount(0), totalDefragmentationTime(0),
                       averageAllocationTime(0.0f), averageDeallocationTime(0.0f),
                       memoryEfficiency(0.0f), allocationSuccessRate(1.0f), 
                       failedAllocationCount(0), allocationCount(0), deallocationCount(0) {}
};

/**
 * @brief RHI自适应内存池类
 * @details 继承自RHIMemoryPool，提供智能自适应内存管理功能
 */
class RHIAdaptiveMemoryPool : public RHIMemoryPool {
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param desc 基础内存池描述符
     * @param config 自适应配置参数
     */
    explicit RHIAdaptiveMemoryPool(RHIDeviceBase& device, const MemoryPoolDesc& desc, 
                                  const AdaptiveConfig& config = AdaptiveConfig());
    
    /**
     * @brief 析构函数
     */
    ~RHIAdaptiveMemoryPool() override;
    
    // === 禁用拷贝，支持移动 ===
    
    DISABLE_COPY(RHIAdaptiveMemoryPool);
    DISABLE_MOVE(RHIAdaptiveMemoryPool);
    
    // === RHIMemoryPool接口实现 ===
    
    /**
     * @brief 初始化自适应内存池
     * @return 初始化是否成功
     */
    bool Initialize() override;
    
    /**
     * @brief 分配内存块（增强版）
     * @param size 需要分配的大小
     * @param alignment 对齐要求
     * @param usage 内存用途
     * @return 内存块句柄，失败返回0
     */
    u32 Allocate(u64 size, u64 alignment = 0, GPUMemoryUsage usage = GPUMemoryUsage::Unknown) override;
    
    /**
     * @brief 释放内存块（增强版）
     * @param blockHandle 内存块句柄
     * @return 释放是否成功
     */
    bool Deallocate(u32 blockHandle) override;
    
    /**
     * @brief 重新分配内存块（增强版）
     * @param blockHandle 内存块句柄
     * @param newSize 新的大小
     * @param newAlignment 新的对齐要求
     * @return 新的内存块句柄，失败返回0
     */
    u32 Reallocate(u32 blockHandle, u64 newSize, u64 newAlignment = 0) override;
    
    /**
     * @brief 内存池整理（智能碎片整理）
     * @return 整理是否成功
     */
    bool Defragment() override;
    
    /**
     * @brief 清空内存池
     * @return 清空是否成功
     */
    bool Clear() override;
    
    /**
     * @brief 获取内存块信息
     * @param blockHandle 内存块句柄
     * @return 内存块描述符，失败返回空对象
     */
    MemoryBlock GetMemoryBlock(u32 blockHandle) const override;
    
    /**
     * @brief 验证内存块有效性
     * @param blockHandle 内存块句柄
     * @return 内存块是否有效
     */
    bool IsValidBlock(u32 blockHandle) const override;
    
    /**
     * @brief 验证内存池一致性
     * @return 验证是否通过
     */
    bool Validate() const override;
    
    // === 自适应功能接口 ===
    
    /**
     * @brief 启动自适应分析
     * @return 是否启动成功
     */
    bool StartAdaptiveAnalysis();
    
    /**
     * @brief 停止自适应分析
     */
    void StopAdaptiveAnalysis();
    
    /**
     * @brief 手动触发模式分析
     * @return 检测到的使用模式
     */
    MemoryUsagePattern AnalyzeUsagePattern();
    
    /**
     * @brief 手动触发自适应调整
     * @return 调整是否成功
     */
    bool PerformAdaptiveAdjustment();
    
    /**
     * @brief 手动触发热点分析
     * @return 热点内存块数量
     */
    u32 AnalyzeHotspots();
    
    /**
     * @brief 预分配热点内存
     * @param hotspotCount 需要预分配的热点数量
     * @return 预分配是否成功
     */
    bool PreallocateHotspots(u32 hotspotCount);
    
    // === 访问器方法 ===
    
    /**
     * @brief 获取自适应配置
     * @return 配置的常量引用
     */
    const AdaptiveConfig& GetAdaptiveConfig() const { return config_; }
    
    /**
     * @brief 设置自适应配置
     * @param config 新的配置
     */
    void SetAdaptiveConfig(const AdaptiveConfig& config);
    
    /**
     * @brief 获取自适应指标
     * @return 指标的常量引用
     */
    const AdaptiveMetrics& GetAdaptiveMetrics() const { return metrics_; }
    
    /**
     * @brief 获取当前使用模式
     * @return 使用模式
     */
    MemoryUsagePattern GetCurrentPattern() const { return metrics_.detectedPattern; }
    
    /**
     * @brief 获取模式置信度
     * @return 置信度（0.0-1.0）
     */
    f32 GetPatternConfidence() const { return metrics_.patternConfidence; }
    
    /**
     * @brief 检查自适应分析是否运行中
     * @return 是否运行中
     */
    bool IsAdaptiveAnalysisRunning() const { return adaptiveAnalysisRunning_; }
    
    /**
     * @brief 获取热点内存块列表
     * @return 热点内存块句柄列表
     */
    const primal::utl::vector<u32>& GetHotspotBlocks() const { return hotspotBlocks_; }
    
    // === 调试和诊断方法 ===
    
    /**
     * @brief 打印自适应分析报告
     */
    void PrintAdaptiveReport() const;
    
    /**
     * @brief 生成自适应分析详细报告
     * @return 报告字符串
     */
    std::string GenerateAdaptiveReport() const;
    
    /**
     * @brief 导出使用模式数据
     * @param filename 导出文件名
     * @return 导出是否成功
     */
    bool ExportUsagePatternData(const char* filename) const;
    
    /**
     * @brief 重置自适应指标
     */
    void ResetAdaptiveMetrics();

protected:
    // === 受保护的虚函数实现 ===
    
    void destroyImpl() override;
    void updateStats() override;
    
private:
    // === 私有成员变量 ===
    
    AdaptiveConfig config_;                        ///< 自适应配置
    AdaptiveMetrics metrics_;                      ///< 自适应指标
    bool adaptiveAnalysisRunning_;                 ///< 自适应分析运行标志
    
    // 使用模式分析
    std::deque<UsageWindow> usageWindows_;          ///< 使用窗口历史
    u64 analysisStartTime_;                         ///< 分析开始时间
    u64 lastPatternAnalysis_;                       ///< 上次模式分析时间
    
    // 实际内存管理
    primal::utl::vector<MemoryBlock> memoryBlocks_; ///< 内存块列表
    primal::utl::vector<u32> freeBlocks_;           ///< 空闲块列表
    u64 nextBlockHandle_;                            ///< 下一个块句柄
    
    // 分配记录跟踪
    std::unordered_map<u32, AllocationRecord> allocationRecords_; ///< 分配记录映射
    primal::utl::vector<u32> activeAllocations_;   ///< 活跃分配列表
    u64 totalAllocationTime_;                       ///< 总分配时间
    u64 totalDeallocationTime_;                     ///< 总释放时间
    
    // 热点内存管理
    primal::utl::vector<u32> hotspotBlocks_;        ///< 热点内存块列表
    std::unordered_map<u32, u64> accessTimestamps_; ///< 访问时间戳映射
    primal::utl::vector<u32> preallocatedBlocks_;    ///< 预分配块列表
    
    // 自适应调整
    u64 lastResizeTime_;                            ///< 上次调整时间
    u64 lastDefragmentationTime_;                   ///< 上次碎片整理时间
    
    // 线程安全
    mutable std::mutex adaptiveMutex_;              ///< 自适应操作互斥锁
    mutable std::mutex allocationMutex_;            ///< 分配操作互斥锁
    mutable std::mutex metricsMutex_;               ///< 指标更新互斥锁
    
    // === 私有辅助方法 ===
    
    // 模式分析相关
    MemoryUsagePattern detectUsagePattern(const std::deque<UsageWindow>& windows);
    f32 calculatePatternConfidence(MemoryUsagePattern pattern, const std::deque<UsageWindow>& windows);
    void updateUsageWindow();
    void recordAllocation(u32 blockHandle, u64 size, u64 alignment, GPUMemoryUsage usage);
    void recordDeallocation(u32 blockHandle);
    
    // 热点分析相关
    void updateHotspotStatus(u32 blockHandle);
    MemoryHotspotLevel calculateHotspotLevel(const AllocationRecord& record);
    bool isHotspot(u32 blockHandle) const;
    
    // 自适应调整相关
    bool shouldResizePool();
    bool shouldDefragment();
    u64 calculateOptimalPoolSize();
    bool resizePool(u64 newSize);
    void updateHotspotPreallocation();
    
    // 性能监控相关
    void updateAllocationMetrics(u64 allocationTime);
    void updateDeallocationMetrics(u64 deallocationTime);
    void updateMemoryEfficiency();
    
    // 工具方法
    u64 getCurrentTimestamp() const;
    u32 findFreePreallocatedBlock(u64 size, u64 alignment);
    u32 allocateFromMemoryPool(u64 size, u64 alignment, GPUMemoryUsage usage);
    void deallocateFromMemoryPool(u32 blockHandle);
    void addToPreallocatedPool(u32 blockHandle);
    void removeFromPreallocatedPool(u32 blockHandle);
    
    // 验证和调试方法
    bool validateAllocationRecords() const;
    bool validateHotspotConsistency() const;
    void logAdaptiveEvent(const char* event, const char* details = nullptr) const;
};

} // namespace primal::graphics::rhi