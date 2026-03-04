/**
 * @file RHIDeterministicPrefetch.h
 * @brief 确定性资源预取系统 - 基于静态规则和硬件分析
 * @details 完全避免机器学习，使用确定性算法进行资源预取决策
 * 
 * 设计原则：
 * 1. 运行时严格避免ML/AI算法
 * 2. 基于静态规则引擎进行决策
 * 3. 硬件能力分析和访问模式统计
 * 4. 时间窗口管理和内存预算控制
 * 
 * @author Game Engine Team
 * @date 2025-01-04
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"
#include <array>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <memory>

namespace primal::graphics::rhi {

// 编译开关：确定性预取系统（默认启用）
#ifndef RHI_DETERMINISTIC_PREFETCH_ENABLED
#define RHI_DETERMINISTIC_PREFETCH_ENABLED 1
#endif

// 编译开关：ML功能（实验性，默认禁用）
#ifndef RHI_ML_FEATURES_ENABLED
#define RHI_ML_FEATURES_ENABLED 0
#endif

// 前向声明
class RHIBuffer;
class RHITexture;

/**
 * @brief 预取资源类型枚举
 */
enum class PrefetchResourceType : u8 {
    Geometry,           // 几何体数据
    Texture,           // 纹理数据
    Shader,            // 着色器
    Animation,         // 动画数据
    Audio,             // 音频数据
    Material,          // 材质数据
    PipelineState,     // 渲染管线状态
    Count
};

/**
 * @brief 场景类型枚举
 */
enum class SceneType : u8 {
    Indoor,            // 室内场景
    Outdoor,           // 室外场景
    Urban,             // 城市场景
    Nature,            // 自然场景
    Combat,            // 战斗场景
    Menu,              // 菜单界面
    Loading,           // 加载场景
    Count
};

/**
 * @brief 硬件等级枚举
 */
enum class HardwareClass : u8 {
    Low,               // 低端硬件
    Medium,            // 中端硬件
    High,              // 高端硬件
    Ultra,             // 顶级硬件
    Count
};

/**
 * @brief 访问类型枚举
 */
enum class AccessType : u8 {
    Sequential,        // 顺序访问
    Random,            // 随机访问
    PatternBased,      // 基于模式访问
    Predictable,       // 可预测访问
    Count
};

/**
 * @brief 时间窗口配置
 */
struct TimeWindow {
    u32 cpuToGpuFrames;        // CPU到GPU数据传输的预取帧数
    u32 renderDataFrames;      // 渲染数据的预取帧数
    u32 cullingDataFrames;     // 剔除/LOD/动画数据的预取帧数
    u32 streamingDataMs;       // 资源流式加载的时间窗口(毫秒)
    
    TimeWindow() 
        : cpuToGpuFrames(3)
        , renderDataFrames(2)
        , cullingDataFrames(3)
        , streamingDataMs(500) {}
};

/**
 * @brief 硬件配置文件
 */
struct HardwareProfile {
    u64 totalGPUMemoryMB;      // GPU总内存(MB)
    u64 systemMemoryMB;        // 系统内存(MB)
    u32 memoryBandwidthGBps;    // 内存带宽(GB/s)
    u32 storageType;           // 存储类型 (0=HDD, 1=SSD, 2=NVMe)
    HardwareClass hardwareClass; // 硬件等级
    bool supportsAsyncCompute;  // 是否支持异步计算
    bool supportsFastStorage;   // 是否支持快速存储
};

/**
 * @brief 访问记录
 */
struct AccessRecord {
    u64 resourceId;             // 资源ID
    u64 timestamp;              // 访问时间戳
    u32 frameNumber;            // 帧号
    PrefetchResourceType resourceType;  // 资源类型
    u32 lodLevel;               // LOD级别
    AccessType accessType;      // 访问类型
    u32 accessCount;            // 访问次数
};

/**
 * @brief 访问频率统计
 */
struct AccessFrequency {
    u64 resourceId;             // 资源ID
    u32 accessCount;            // 总访问次数
    u64 lastAccessTime;         // 最后访问时间
    u64 firstAccessTime;        // 首次访问时间
    f32 averageInterval;        // 平均访问间隔(秒)
    AccessType patternType;     // 访问模式类型
    bool isHotResource;         // 是否为热点资源
};

/**
 * @brief 预取规则
 */
struct PrefetchRule {
    PrefetchResourceType resourceType;  // 资源类型
    SceneType sceneType;        // 场景类型
    HardwareClass hardwareClass; // 硬件等级
    TimeWindow timeWindow;      // 时间窗口
    u32 priority;               // 优先级 (0-100)
    f32 confidence;             // 置信度 (0.0-1.0)
    u32 memoryBudgetMB;         // 内存预算(MB)
    bool enabled;               // 是否启用
};

/**
 * @brief 预取决策
 */
struct PrefetchDecision {
    u64 resourceId;             // 资源ID
    PrefetchResourceType resourceType;  // 资源类型
    u32 targetLOD;              // 目标LOD级别
    u32 frameOffset;            // 预取帧偏移
    u32 priority;               // 优先级
    f32 confidence;             // 置信度
    u64 estimatedSize;          // 预估大小(字节)
    bool shouldPrefetch;        // 是否应该预取
};

/**
 * @brief 预取配置
 */
struct PrefetchConfiguration {
    bool enableDeterministic;   // 启用确定性预取
    bool enableMLFeatures;      // 启用ML功能(实验性)
    u64 maxMemoryBudgetMB;      // 最大内存预算(MB)
    u32 historyWindowSize;      // 历史记录窗口大小
    u32 cleanupIntervalSeconds; // 清理间隔(秒)
    f32 hotResourceThreshold;   // 热点资源阈值
    bool enableLogging;         // 启用日志记录
    bool enableProfiling;       // 启用性能分析
};

/**
 * @brief 帧数据
 */
struct FrameData {
    u32 currentFrameNumber;      // 当前帧号
    SceneType currentSceneType; // 当前场景类型
    f32 frameTime;              // 帧时间(毫秒)
    u64 currentMemoryUsageMB;   // 当前内存使用量(MB)
    utl::vector<u64> visibleResources; // 可见资源ID列表
};

/**
 * @brief 循环缓冲区模板
 */
template<typename T, size_t Size>
class CircularBuffer {
private:
    std::array<T, Size> buffer_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    mutable std::mutex mutex_;

public:
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_[head_] = item;
        head_ = (head_ + 1) % Size;
        if (count_ < Size) {
            ++count_;
        } else {
            tail_ = (tail_ + 1) % Size;
        }
        return true;
    }
    
    bool pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return false;
        
        item = buffer_[tail_];
        tail_ = (tail_ + 1) % Size;
        --count_;
        return true;
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = tail_ = count_ = 0;
    }
};

namespace deterministic_prefetch {

/**
 * @brief 硬件能力分析器
 */
class HardwareCapabilityAnalyzer {
private:
    HardwareProfile hardwareProfile_;
    bool isInitialized_;
    
public:
    HardwareCapabilityAnalyzer();
    ~HardwareCapabilityAnalyzer() = default;
    
    // 初始化硬件分析
    bool Initialize();
    
    // 检测GPU内存大小
    u64 DetectGPUMemorySize();
    
    // 测量内存带宽
    u32 MeasureMemoryBandwidth();
    
    // 测量加载延迟
    u32 MeasureLoadLatency();
    
    // 检测异步加载支持
    bool DetectAsyncLoadSupport();
    
    // 检测存储类型
    u32 DetectStorageType();
    
    // 计算性能等级
    u32 CalculatePerformanceTier();
    
    // 确定硬件等级
    HardwareClass DetermineHardwareClass();
    
    // 获取硬件配置文件
    const HardwareProfile& GetProfile() const { return hardwareProfile_; }
    
    // 更新硬件配置文件
    void UpdateProfile(const HardwareProfile& profile) { hardwareProfile_ = profile; }
};

/**
 * @brief 访问模式分析器
 */
class AccessPatternAnalyzer {
private:
    std::unordered_map<u64, AccessFrequency> frequencyTable_;
    CircularBuffer<AccessRecord, 10000> accessHistory_;
    PrefetchConfiguration config_;
    
public:
    AccessPatternAnalyzer(const PrefetchConfiguration& config);
    ~AccessPatternAnalyzer() = default;
    
    // 添加访问记录
    void AddAccessRecord(const AccessRecord& record);
    
    // 更新访问历史
    void UpdateAccessHistory();
    
    // 获取访问频率
    AccessFrequency GetAccessFrequency(u64 resourceId) const;
    
    // 分析访问模式
    AccessType AnalyzePattern(u64 resourceId);
    
    // 清理过期记录
    void CleanupExpiredRecords();
    
    // 更新频率表
    void UpdateFrequencyTable(u64 resourceId, u64 timestamp);
    
    // 计算频率等级
    u32 CalculateFrequencyTier(const AccessFrequency& freq);
    
    // 获取热点资源列表
    utl::vector<u64> GetHotResources(size_t maxCount = 100) const;
    
    // 获取所有频率数据
    const std::unordered_map<u64, AccessFrequency>& GetAllFrequencies() const { return frequencyTable_; }
};

/**
 * @brief 静态规则引擎
 */
class StaticRuleEngine {
private:
    utl::vector<PrefetchRule> rules_;
    bool isInitialized_;
    
public:
    StaticRuleEngine();
    ~StaticRuleEngine() = default;
    
    // 初始化规则引擎
    bool Initialize();
    
    // 添加规则
    void AddRule(const PrefetchRule& rule);
    
    // 评估规则
    utl::vector<PrefetchRule> EvaluateRules(PrefetchResourceType resourceType, 
                                          SceneType sceneType,
                                          HardwareClass hardwareClass);
    
    // 检查规则匹配
    bool RuleMatches(const PrefetchRule& rule, 
                     PrefetchResourceType resourceType, 
                     SceneType sceneType, 
                     HardwareClass hardwareClass);
    
    // 初始化默认规则
    void InitializeDefaultRules();
    
    // 获取所有规则
    const utl::vector<PrefetchRule>& GetAllRules() const { return rules_; }
    
    // 清空规则
    void ClearRules() { rules_.clear(); }
    
    // 根据条件生成候选资源
    utl::vector<u64> GenerateCandidates(const FrameData& frameData, 
                                       const HardwareProfile& profile,
                                       const AccessPatternAnalyzer& patternAnalyzer);
};

/**
 * @brief 确定性预取管理器
 */
class RHIDeterministicPrefetchManager {
private:
    std::unique_ptr<deterministic_prefetch::HardwareCapabilityAnalyzer> hardwareAnalyzer_;
    std::unique_ptr<AccessPatternAnalyzer> patternAnalyzer_;
    std::unique_ptr<StaticRuleEngine> ruleEngine_;
    
    PrefetchConfiguration config_;
    bool isInitialized_;
    u64 currentMemoryUsage_;
    utl::vector<PrefetchDecision> activeDecisions_;
    
public:
    explicit RHIDeterministicPrefetchManager(const PrefetchConfiguration& config);
    ~RHIDeterministicPrefetchManager() = default;
    
    // 初始化预取管理器
    bool Initialize();
    
    // 更新预取管理器
    void Update(const FrameData& frameData);
    
    // 关闭预取管理器
    void Shutdown();
    
    // 生成预取决策
    utl::vector<PrefetchDecision> GeneratePrefetchDecisions(const FrameData& frameData);
    
    // 调度异步加载
    void ScheduleAsyncLoads(const utl::vector<PrefetchDecision>& decisions);
    
    // 监控和调整
    void MonitorAndAdjust();
    
    // 检查内存预算
    bool HasMemoryBudget(u64 requestedSize);
    
    // 计算确定性置信度
    f32 CalculateDeterministicConfidence(u64 resourceId, 
                                         PrefetchResourceType resourceType, 
                                         const FrameData& frameData);
    
    // 调度异步加载
    bool ScheduleAsyncLoad(u64 resourceId, PrefetchResourceType resourceType, u32 priority);
    
    // 获取当前配置
    const PrefetchConfiguration& GetConfiguration() const { return config_; }
    
    // 更新配置
    void UpdateConfiguration(const PrefetchConfiguration& config) { config_ = config; }
    
    // 获取硬件分析器
    deterministic_prefetch::HardwareCapabilityAnalyzer* GetHardwareAnalyzer() const { return hardwareAnalyzer_.get(); }
    
    // 获取模式分析器
    AccessPatternAnalyzer* GetPatternAnalyzer() const { return patternAnalyzer_.get(); }
    
    // 获取规则引擎
    StaticRuleEngine* GetRuleEngine() const { return ruleEngine_.get(); }
    
    // 获取活跃决策
    const utl::vector<PrefetchDecision>& GetActiveDecisions() const { return activeDecisions_; }
    
    // 获取当前内存使用量（公共接口）
    u64 GetCurrentMemoryUsage() const;
    
    // 估算资源大小
    u64 EstimateResourceSize(PrefetchResourceType type, u64 resourceId);

private:
};

#if RHI_ML_FEATURES_ENABLED
/**
 * @brief 机器学习预测系统接口（实验性功能）
 * @note 仅在编译开关启用时可用，核心系统不依赖
 */
class IMLPredictiveSystem {
public:
    virtual ~IMLPredictiveSystem() = default;
    
    // 初始化ML系统
    virtual bool Initialize() = 0;
    
    // 预测下一个访问资源
    virtual utl::vector<u64> PredictNextResources(const FrameData& frameData) = 0;
    
    // 更新模型
    virtual void UpdateModel(const AccessRecord& record) = 0;
    
    // 获取预测置信度
    virtual f32 GetPredictionConfidence(u64 resourceId) = 0;
    
    // 关闭ML系统
    virtual void Shutdown() = 0;
};
#endif

// 性能统计结构体
struct PerformanceStats {
    u64 totalPredictions;
    u64 accuratePredictions;
    f32 accuracyRate;
    u64 memoryUsageMB;
    f32 averageLatency;
};



/**
 * @brief 确定性预取系统接口
 * @details 核心接口，不依赖任何ML组件
 */
class IDeterministicPrefetchSystem {
public:
    virtual ~IDeterministicPrefetchSystem() = default;
    
    // 创建预取系统实例
    static std::unique_ptr<IDeterministicPrefetchSystem> Create(const PrefetchConfiguration& config = {});
    
    // 初始化系统
    virtual bool Initialize() = 0;
    
    // 更新系统
    virtual void Update(const FrameData& frameData) = 0;
    
    // 关闭系统
    virtual void Shutdown() = 0;
    
    // 处理资源访问
    virtual void OnResourceAccess(u64 resourceId, PrefetchResourceType type, u32 lodLevel) = 0;
    
    // 获取预取建议
    virtual utl::vector<PrefetchDecision> GetPrefetchSuggestions() = 0;
    
    // 设置场景类型
    virtual void SetSceneType(SceneType sceneType) = 0;
    
    // 获取系统状态
    virtual bool IsHealthy() const = 0;
    
    // 获取性能统计
    virtual PerformanceStats GetPerformanceStats() const = 0;
};

// 工厂函数
std::unique_ptr<RHIDeterministicPrefetchManager> CreateDeterministicPrefetchManager(
    const PrefetchConfiguration& config = {});

// === 工具函数 ===

/**
 * @brief 获取系统内存大小
 * @return 系统内存大小（MB）
 */
u64 GetSystemMemorySize();

/**
 * @brief 获取当前时间戳
 * @return 时间戳（微秒）
 */
u64 GetCurrentTimestamp();

// === 工具函数 ===
/**
 * @brief 获取系统内存大小
 * @return 系统内存大小（MB）
 */
u64 GetSystemMemorySize();

/**
 * @brief 获取当前时间戳
 * @return 时间戳（微秒）
 */
u64 GetCurrentTimestamp();
} // namespace deterministic_prefetch
} // namespace primal::graphics::rhi