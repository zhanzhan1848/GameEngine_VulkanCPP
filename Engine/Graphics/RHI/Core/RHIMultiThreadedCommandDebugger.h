/**
 * @file RHIMultiThreadedCommandDebugger.h
 * @brief RHI多线程命令生成器智能调试工具
 * @details 提供开发时诊断、性能分析和优化建议功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"
#include "RHIMultiThreadedCommandGenerator.h"
// #include <vector>  // Replaced with Utilities/Vector.h via CommonHeaders.h
#include <string>
#include <memory>

namespace primal::graphics::rhi {

// === 前向声明 ===
class RHIDeviceBase;

/**
 * @brief 调试级别枚举
 * @details 定义不同级别的调试信息详细程度
 */
enum class DebugLevel : u8 {
    Basic = 0,      ///< 基础调试信息
    Detailed = 1,   ///< 详细调试信息
    Verbose = 2,    ///< 冗长调试信息
    Profiling = 3   ///< 性能分析信息
};

/**
 * @brief 优化建议类型枚举
 */
enum class OptimizationType : u8 {
    None = 0,           ///< 无建议
    ThreadCount = 1,    ///< 线程数量优化
    MemoryUsage = 2,    ///< 内存使用优化
    TaskDistribution = 3, ///< 任务分发优化
    CacheStrategy = 4,   ///< 缓存策略优化
    Synchronization = 5, ///< 同步机制优化
    BatchSize = 6        ///< 批次大小优化
};

/**
 * @brief 性能瓶颈类型
 */
enum class BottleneckType : u8 {
    None = 0,           ///< 无瓶颈
    CPU = 1,            ///< CPU瓶颈
    Memory = 2,         ///< 内存瓶颈
    Synchronization = 3, ///< 同步瓶颈
    I_O = 4,            ///< I/O瓶颈
    Cache = 5           ///< 缓存瓶颈
};

/**
 * @brief 调试信息结构
 * @details 包含系统当前状态的调试信息
 */
struct DebugInfo {
    // 系统状态
    bool isInitialized;                    ///< 是否已初始化
    u32 workerThreadCount;                 ///< 工作线程数量
    u32 activeThreads;                     ///< 活跃线程数量
    u32 pendingTasks;                      ///< 待处理任务数量
    u64 totalProcessedTasks;               ///< 总处理任务数量
    
    // 性能指标
    f64 averageProcessingTimeMs;           ///< 平均处理时间
    f64 maxProcessingTimeMs;               ///< 最大处理时间
    f64 minProcessingTimeMs;               ///< 最小处理时间
    f64 cpuUtilizationPercent;             ///< CPU利用率
    f64 memoryUsageMB;                     ///< 内存使用量（MB）
    f64 cacheHitRate;                      ///< 缓存命中率
    
    // 错误统计
    u64 totalErrors;                       ///< 总错误数
    u64 recentErrors;                      ///< 最近错误数
    utl::vector<std::string> errorMessages; ///< 错误消息列表
    
    /**
     * @brief 构造函数
     */
    DebugInfo() : isInitialized(false), workerThreadCount(0), activeThreads(0),
                 pendingTasks(0), totalProcessedTasks(0),
                 averageProcessingTimeMs(0.0), maxProcessingTimeMs(0.0), minProcessingTimeMs(0.0),
                 cpuUtilizationPercent(0.0), memoryUsageMB(0.0), cacheHitRate(0.0),
                 totalErrors(0), recentErrors(0) {}
};

/**
 * @brief 优化建议结构
 * @details 包含具体的优化建议和预期效果
 */
struct OptimizationSuggestion {
    OptimizationType type;                ///< 建议类型
    std::string title;                     ///< 建议标题
    std::string description;               ///< 详细描述
    std::string implementation;            ///< 实现建议
    f64 expectedImprovement;               ///< 预期性能提升百分比
    u32 priority;                          ///< 优先级（1-10）
    bool isApplicable;                     ///< 是否适用
    
    /**
     * @brief 构造函数
     */
    OptimizationSuggestion() : type(OptimizationType::None), expectedImprovement(0.0),
                               priority(5), isApplicable(false) {}
};

/**
 * @brief 性能分析报告
 * @details 包含详细的性能分析结果
 */
struct PerformanceAnalysisReport {
    // 总体评估
    f64 overallPerformanceScore;           ///< 总体性能评分（0-100）
    BottleneckType primaryBottleneck;      ///< 主要瓶颈类型
    std::string bottleneckDescription;     ///< 瓶颈描述
    
    // 详细分析
    utl::vector<OptimizationSuggestion> suggestions; ///< 优化建议列表
    DebugInfo currentDebugInfo;            ///< 当前调试信息
    utl::vector<std::string> warnings;     ///< 警告列表
    utl::vector<std::string> recommendations; ///< 推荐操作列表
    
    // 历史对比
    f64 performanceTrend;                  ///< 性能趋势（百分比变化）
    utl::vector<f64> historicalPerformance; ///< 历史性能数据
    
    /**
     * @brief 构造函数
     */
    PerformanceAnalysisReport() : overallPerformanceScore(0.0), 
                                primaryBottleneck(BottleneckType::None),
                                performanceTrend(0.0) {}
};

/**
 * @brief 调试配置结构
 */
struct DebugConfig {
    DebugLevel debugLevel;                  ///< 调试级别
    bool enableRealTimeMonitoring;          ///< 是否启用实时监控
    bool enablePerformanceProfiling;        ///< 是否启用性能分析
    bool enableOptimizationSuggestions;    ///< 是否启用优化建议
    bool enableHistoricalTracking;          ///< 是否启用历史跟踪
    u32 maxHistoryEntries;                  ///< 最大历史条目数
    u32 profilingIntervalMs;                ///< 分析间隔（毫秒）
    bool autoGenerateReports;               ///< 是否自动生成报告
    std::string reportOutputPath;            ///< 报告输出路径
    
    /**
     * @brief 构造函数
     */
    DebugConfig() : debugLevel(DebugLevel::Basic),
                   enableRealTimeMonitoring(true),
                   enablePerformanceProfiling(true),
                   enableOptimizationSuggestions(true),
                   enableHistoricalTracking(true),
                   maxHistoryEntries(100),
                   profilingIntervalMs(1000),
                   autoGenerateReports(false),
                   reportOutputPath("./debug_reports/") {}
};

/**
 * @brief RHI多线程命令生成器智能调试工具
 * @details 提供开发时诊断、性能分析和智能优化建议
 */
class RHIMultiThreadedCommandDebugger {
public:
    /**
     * @brief 构造函数
     * @param generator 多线程命令生成器引用
     * @param config 调试配置
     */
    explicit RHIMultiThreadedCommandDebugger(RHIMultiThreadedCommandGenerator& generator,
                                             const DebugConfig& config = DebugConfig{});
    
    /**
     * @brief 析构函数
     */
    ~RHIMultiThreadedCommandDebugger();
    
    // === 初始化和控制接口 ===
    
    /**
     * @brief 初始化调试器
     * @return 是否初始化成功
     */
    bool Initialize();
    
    /**
     * @brief 关闭调试器
     */
    void Shutdown();
    
    /**
     * @brief 更新调试配置
     * @param config 新的调试配置
     */
    void UpdateConfig(const DebugConfig& config);
    
    /**
     * @brief 获取当前调试配置
     * @return 当前调试配置
     */
    const DebugConfig& GetConfig() const;
    
    // === 调试信息获取接口 ===
    
    /**
     * @brief 获取当前调试信息
     * @param level 调试级别
     * @return 调试信息
     */
    DebugInfo GetCurrentDebugInfo(DebugLevel level = DebugLevel::Basic) const;
    
    /**
     * @brief 获取格式化的调试报告
     * @param buffer 输出缓冲区
     * @param bufferSize 缓冲区大小
     * @param level 调试级别
     * @return 实际写入的字符数
     */
    u32 GetFormattedDebugReport(char* buffer, u32 bufferSize, DebugLevel level = DebugLevel::Basic) const;
    
    /**
     * @brief 导出调试报告到文件
     * @param filename 文件名
     * @param level 调试级别
     * @return 是否导出成功
     */
    bool ExportDebugReport(const char* filename, DebugLevel level = DebugLevel::Basic) const;
    
    // === 性能分析接口 ===
    
    /**
     * @brief 执行性能分析
     * @return 性能分析报告
     */
    PerformanceAnalysisReport PerformPerformanceAnalysis();
    
    /**
     * @brief 获取优化建议
     * @return 优化建议列表
     */
    utl::vector<OptimizationSuggestion> GetOptimizationSuggestions() const;
    
    /**
     * @brief 获取主要瓶颈类型
     * @return 瓶颈类型
     */
    BottleneckType GetPrimaryBottleneck() const;
    
    /**
     * @brief 计算性能评分
     * @return 性能评分（0-100）
     */
    f64 CalculatePerformanceScore() const;
    
    // === 实时监控接口 ===
    
    /**
     * @brief 启用实时监控
     */
    void EnableRealTimeMonitoring();
    
    /**
     * @brief 禁用实时监控
     */
    void DisableRealTimeMonitoring();
    
    /**
     * @brief 获取实时监控状态
     * @return 是否启用实时监控
     */
    bool IsRealTimeMonitoringEnabled() const;
    
    /**
     * @brief 设置监控回调函数
     * @param callback 回调函数
     */
    void SetMonitoringCallback(std::function<void(const DebugInfo&)> callback);
    
    // === 历史数据接口 ===
    
    /**
     * @brief 获取历史性能数据
     * @return 历史数据列表
     */
    utl::vector<f64> GetHistoricalPerformanceData() const;
    
    /**
     * @brief 清除历史数据
     */
    void ClearHistoricalData();
    
    /**
     * @brief 导出历史数据
     * @param filename 文件名
     * @return 是否导出成功
     */
    bool ExportHistoricalData(const char* filename) const;
    
    // === 智能诊断接口 ===
    
    /**
     * @brief 自动诊断系统问题
     * @return 诊断结果描述
     */
    std::string AutoDiagnoseSystem() const;
    
    /**
     * @brief 验证系统配置
     * @return 验证结果和问题列表
     */
    std::pair<bool, utl::vector<std::string>> ValidateSystemConfiguration() const;
    
    /**
     * @brief 推荐最佳配置
     * @return 推荐的配置参数
     */
    MultiThreadConfig RecommendOptimalConfiguration() const;
    
    // === 调试工具接口 ===
    
    /**
     * @brief 开始性能分析会话
     */
    void BeginProfilingSession();
    
    /**
     * @brief 结束性能分析会话
     * @return 分析结果
     */
    PerformanceAnalysisReport EndProfilingSession();
    
    /**
     * @brief 生成性能快照
     * @param label 快照标签
     */
    void TakePerformanceSnapshot(const char* label);
    
    /**
     * @brief 比较性能快照
     * @param label1 第一个快照标签
     * @param label2 第二个快照标签
     * @return 比较结果
     */
    std::string ComparePerformanceSnapshots(const char* label1, const char* label2) const;
    
private:
    // === 内部分析方法 ===
    
    /**
     * @brief 分析线程效率
     * @param debugInfo 调试信息
     * @return 线程效率评分
     */
    f64 AnalyzeThreadEfficiency(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 分析内存使用
     * @param debugInfo 调试信息
     * @return 内存使用评分
     */
    f64 AnalyzeMemoryUsage(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 分析缓存效率
     * @param debugInfo 调试信息
     * @return 缓存效率评分
     */
    f64 AnalyzeCacheEfficiency(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 生成线程优化建议
     * @param debugInfo 调试信息
     * @return 优化建议
     */
    OptimizationSuggestion GenerateThreadOptimization(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 生成内存优化建议
     * @param debugInfo 调试信息
     * @return 优化建议
     */
    OptimizationSuggestion GenerateMemoryOptimization(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 生成缓存优化建议
     * @param debugInfo 调试信息
     * @return 优化建议
     */
    OptimizationSuggestion GenerateCacheOptimization(const DebugInfo& debugInfo) const;
    
    /**
     * @brief 更新历史数据
     * @param performanceScore 当前性能评分
     */
    void UpdateHistoricalData(f64 performanceScore);
    
    /**
     * @brief 实时监控线程函数
     */
    void RealTimeMonitoringThreadFunction();
    
    /**
     * @brief 格式化优化建议
     * @param suggestion 优化建议
     * @return 格式化字符串
     */
    std::string FormatOptimizationSuggestion(const OptimizationSuggestion& suggestion) const;
    
    // === 成员变量 ===
    
    RHIMultiThreadedCommandGenerator& generator_;  ///< 多线程命令生成器引用
    DebugConfig config_;                            ///< 调试配置
    
    std::atomic<bool> isInitialized_;               ///< 是否已初始化
    std::atomic<bool> isMonitoring_;                ///< 是否正在监控
    std::atomic<bool> isShuttingDown_;              ///< 是否正在关闭
    
    // 实时监控
    std::thread monitoringThread_;                  ///< 监控线程
    std::function<void(const DebugInfo&)> monitoringCallback_; ///< 监控回调函数
    
    // 历史数据
    utl::vector<f64> performanceHistory_;           ///< 性能历史数据
    utl::vector<std::chrono::system_clock::time_point> timestamps_; ///< 时间戳
    mutable std::mutex historyMutex_;              ///< 历史数据互斥锁
    
    // 性能快照
    std::unordered_map<std::string, DebugInfo> performanceSnapshots_; ///< 性能快照
    mutable std::mutex snapshotsMutex_;            ///< 快照互斥锁
    
    // 分析会话
    bool isInProfilingSession_;                     ///< 是否在分析会话中
    std::chrono::system_clock::time_point profilingStartTime_; ///< 分析开始时间
    DebugInfo profilingStartInfo_;                  ///< 分析开始时的调试信息
    
    // 配置互斥锁
    mutable std::mutex configMutex_;               ///< 配置互斥锁
    
    // 禁用拷贝和移动
    DISABLE_COPY_AND_MOVE(RHIMultiThreadedCommandDebugger);
};

} // namespace primal::graphics::rhi