/**
 * @file RHIDeterministicPrefetch.cpp
 * @brief 确定性资源预取系统实现
 * @details 基于静态规则和硬件分析的确定性资源预取
 * 
 * @author Game Engine Team
 * @date 2025-01-04
 */

// 先包含标准库头文件
#include <algorithm>
#include <thread>
#include <iostream>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cstring>

// 再包含项目头文件
#include "RHIDeterministicPrefetch.h"
#include "RHIDevice.h"
#include "RHIMpscQueue.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

// 类型别名
using f64 = double;

namespace primal::graphics::rhi {

// ============================================================================
// 工具函数实现
// ============================================================================

/**
 * @brief 获取系统内存大小
 * @return 系统内存大小（MB）
 */
u64 GetSystemMemorySize() {
#if defined(__APPLE__)
    // macOS系统内存检测
    int64_t memSize = 0;
    size_t size = sizeof(memSize);
    if (sysctlbyname("hw.memsize", &memSize, &size, nullptr, 0) == 0) {
        return static_cast<u64>(memSize) / (1024 * 1024); // 转换为MB
    }
#elif defined(_WIN32)
    // Windows实现
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<u64>(status.ullTotalPhys) / (1024 * 1024);
    }
#else
    // Linux实现
    FILE* file = fopen("/proc/meminfo", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                u64 memKB;
                sscanf(line, "MemTotal: %llu kB", &memKB);
                fclose(file);
                return memKB / 1024; // 转换为MB
            }
        }
        fclose(file);
    }
#endif
    return 8192; // 默认8GB
}

/**
 * @brief 获取当前时间戳
 * @return 时间戳（微秒）
 */
u64 GetCurrentTimestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

// ============================================================================
// HardwareCapabilityAnalyzer 实现
// ============================================================================

namespace deterministic_prefetch {
HardwareCapabilityAnalyzer::HardwareCapabilityAnalyzer() 
    : isInitialized_(false) {
    memset(&hardwareProfile_, 0, sizeof(hardwareProfile_));
}

bool HardwareCapabilityAnalyzer::Initialize() {
    if (isInitialized_) {
        return true;
    }
    
    // 检测GPU内存
    hardwareProfile_.totalGPUMemoryMB = DetectGPUMemorySize();
    
    // 检测系统内存
    hardwareProfile_.systemMemoryMB = GetSystemMemorySize();
    
    // 测量内存带宽
    hardwareProfile_.memoryBandwidthGBps = MeasureMemoryBandwidth();
    
    // 检测存储类型
    hardwareProfile_.storageType = DetectStorageType();
    
    // 检测异步计算支持
    hardwareProfile_.supportsAsyncCompute = DetectAsyncLoadSupport();
    
    // 检测快速存储支持
    hardwareProfile_.supportsFastStorage = (hardwareProfile_.storageType >= 1); // SSD或更高
    
    // 确定硬件等级
    hardwareProfile_.hardwareClass = DetermineHardwareClass();
    
    isInitialized_ = true;
    
    std::cout << "[INFO] Hardware Profile: GPU=" << hardwareProfile_.totalGPUMemoryMB 
           << "MB, System=" << hardwareProfile_.systemMemoryMB 
           << "MB, Bandwidth=" << hardwareProfile_.memoryBandwidthGBps 
           << "GB/s, Class=" << static_cast<int>(hardwareProfile_.hardwareClass) << std::endl;
    
    return true;
}

u64 HardwareCapabilityAnalyzer::DetectGPUMemorySize() {
#if defined(__APPLE__)
    return 4096; // 默认4GB
#else
    // 其他平台的简化实现
    return 8192; // 默认8GB
#endif
}

u32 HardwareCapabilityAnalyzer::MeasureMemoryBandwidth() {
    // 简化的内存带宽测量（实际实现应进行基准测试）
#if defined(__APPLE__)
    // macOS/Metal平台的简化检测
    size_t len = sizeof(u32);
    u32 bandwidth = 25; // 默认25GB/s
    sysctlbyname("hw.memsize", nullptr, &len, nullptr, 0);
    
    // 基于系统内存大小估算带宽
    u64 sysMem = GetSystemMemorySize();
    if (sysMem >= 32768) { // >= 32GB
        bandwidth = 50;
    } else if (sysMem >= 16384) { // >= 16GB
        bandwidth = 35;
    } else if (sysMem >= 8192) { // >= 8GB
        bandwidth = 25;
    } else {
        bandwidth = 15;
    }
    
    return bandwidth;
#else
    return 30; // 默认30GB/s
#endif
}

u32 HardwareCapabilityAnalyzer::MeasureLoadLatency() {
    // 简化的加载延迟测量（毫秒）
    return hardwareProfile_.storageType == 2 ? 1 : // NVMe: 1ms
           hardwareProfile_.storageType == 1 ? 5 : // SSD: 5ms
           10; // HDD: 10ms                        // HDD: 20ms
}

bool HardwareCapabilityAnalyzer::DetectAsyncLoadSupport() {
#if defined(__APPLE__)
    // Metal平台支持异步计算
    return true;
#else
    return true; // 默认支持
#endif
}

u32 HardwareCapabilityAnalyzer::DetectStorageType() {
    // 简化的存储类型检测
    // 实际实现应该检测SSD/NVMe等
    return 1; // 默认SSD
}

u32 HardwareCapabilityAnalyzer::CalculatePerformanceTier() {
    u32 tier = 1; // 基础分数
    
    // GPU内存分数
    if (hardwareProfile_.totalGPUMemoryMB >= 16384) tier += 4;      // >= 16GB
    else if (hardwareProfile_.totalGPUMemoryMB >= 8192) tier += 3;   // >= 8GB
    else if (hardwareProfile_.totalGPUMemoryMB >= 4096) tier += 2;   // >= 4GB
    else if (hardwareProfile_.totalGPUMemoryMB >= 2048) tier += 1;    // >= 2GB
    
    // 内存带宽分数
    if (hardwareProfile_.memoryBandwidthGBps >= 100) tier += 3;
    else if (hardwareProfile_.memoryBandwidthGBps >= 50) tier += 2;
    else if (hardwareProfile_.memoryBandwidthGBps >= 25) tier += 1;
    
    // 存储类型分数
    tier += hardwareProfile_.storageType;
    
    // 异步计算支持
    if (hardwareProfile_.supportsAsyncCompute) tier += 1;
    
    return std::min(tier, 10u); // 最高10级
}


HardwareClass HardwareCapabilityAnalyzer::DetermineHardwareClass() {
    u32 tier = CalculatePerformanceTier();
    
    if (tier >= 8) return HardwareClass::Ultra;
    if (tier >= 6) return HardwareClass::High;
    if (tier >= 4) return HardwareClass::Medium;
    return HardwareClass::Low;
}

// ============================================================================
// AccessPatternAnalyzer 实现
// ============================================================================

AccessPatternAnalyzer::AccessPatternAnalyzer(const PrefetchConfiguration& config)
    : config_(config) {
}

void AccessPatternAnalyzer::AddAccessRecord(const AccessRecord& record) {
    // 添加到历史记录
    accessHistory_.push(record);
    
    // 更新频率表
    UpdateFrequencyTable(record.resourceId, record.timestamp);
}

void AccessPatternAnalyzer::UpdateAccessHistory() {
    // 定期清理过期记录
    if (config_.cleanupIntervalSeconds > 0) {
        CleanupExpiredRecords();
    }
}

AccessFrequency AccessPatternAnalyzer::GetAccessFrequency(u64 resourceId) const {
    auto it = frequencyTable_.find(resourceId);
    if (it != frequencyTable_.end()) {
        return it->second;
    }
    
    // 返回默认频率（不存在的资源ID为0）
    AccessFrequency freq = {};
    freq.resourceId = 0;  // 不存在的资源ID设为0
    return freq;
}

AccessType AccessPatternAnalyzer::AnalyzePattern(u64 resourceId) {
    auto freq = GetAccessFrequency(resourceId);
    
    if (freq.accessCount < 3) {
        return AccessType::Random; // 数据不足，假设随机访问
    }
    
    // 计算访问间隔的变异系数
    [[maybe_unused]] f64 totalInterval = 0.0;
    [[maybe_unused]] u32 validIntervals = 0;
    
    // 简化实现：基于平均访问间隔判断模式
    if (freq.averageInterval < 0.1) { // 100ms内
        return AccessType::Sequential; // 高频访问，可能是顺序访问
    } else if (freq.averageInterval < 1.0) { // 1秒内
        return AccessType::PatternBased; // 中频访问，基于模式
    } else {
        return AccessType::Predictable; // 低频但有规律
    }
}

void AccessPatternAnalyzer::CleanupExpiredRecords() {
    u64 currentTime = GetCurrentTimestamp();
    u64 expireTime = config_.cleanupIntervalSeconds * 1000; // 转换为毫秒
    
    // 清理过期的频率记录
    for (auto it = frequencyTable_.begin(); it != frequencyTable_.end();) {
        if (currentTime - it->second.lastAccessTime > expireTime) {
            it = frequencyTable_.erase(it);
        } else {
            ++it;
        }
    }
}

void AccessPatternAnalyzer::UpdateFrequencyTable(u64 resourceId, u64 timestamp) {
    auto& freq = frequencyTable_[resourceId];
    
    if (freq.resourceId == 0) {
        // 新记录
        freq.resourceId = resourceId;
        freq.accessCount = 1;
        freq.firstAccessTime = timestamp;
        freq.lastAccessTime = timestamp;
        freq.averageInterval = 0.0;
        freq.isHotResource = false;
    } else {
        // 更新现有记录
        freq.accessCount++;
        u64 interval = timestamp - freq.lastAccessTime;
        
        // 计算新的平均间隔
        f64 totalInterval = freq.averageInterval * (freq.accessCount - 2) + interval;
        freq.averageInterval = totalInterval / (freq.accessCount - 1);
        
        freq.lastAccessTime = timestamp;
        
        // 判断是否为热点资源
        freq.isHotResource = (freq.accessCount > config_.hotResourceThreshold);
    }
    
    // 分析访问模式
    freq.patternType = AnalyzePattern(resourceId);
}

u32 AccessPatternAnalyzer::CalculateFrequencyTier(const AccessFrequency& freq) {
    if (freq.accessCount >= 100) return 5;      // 超高频
    if (freq.accessCount >= 50) return 4;       // 高频
    if (freq.accessCount >= 20) return 3;       // 中高频
    if (freq.accessCount >= 10) return 2;       // 中频
    if (freq.accessCount >= 5) return 1;        // 低频
    return 0;                                    // 极低频
}

utl::vector<u64> AccessPatternAnalyzer::GetHotResources(size_t maxCount) const {
    utl::vector<std::pair<u64, u32>> hotResources;
    
    for (const auto& pair : frequencyTable_) {
        if (pair.second.isHotResource) {
            hotResources.emplace_back(pair.first, pair.second.accessCount);
        }
    }
    
    // 按访问次数排序
    std::sort(hotResources.begin(), hotResources.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // 提取资源ID
    utl::vector<u64> result;
    for (size_t i = 0; i < std::min(hotResources.size(), static_cast<u64>(maxCount)); ++i) {
        result.push_back(hotResources[i].first);
    }
    
    return result;
}

// ============================================================================
// StaticRuleEngine 实现
// ============================================================================

StaticRuleEngine::StaticRuleEngine() 
    : isInitialized_(false) {
}

bool StaticRuleEngine::Initialize() {
    if (isInitialized_) {
        return true;
    }
    
    // 初始化默认规则
    InitializeDefaultRules();
    
    isInitialized_ = true;
    return true;
}

void StaticRuleEngine::AddRule(const PrefetchRule& rule) {
    rules_.push_back(rule);
}   

utl::vector<PrefetchRule> StaticRuleEngine::EvaluateRules(
    PrefetchResourceType resourceType, 
    SceneType sceneType, 
    HardwareClass hardwareClass) {
    
    utl::vector<PrefetchRule> matchingRules;
    
    for (const auto& rule : rules_) {
        if (rule.enabled && RuleMatches(rule, resourceType, sceneType, hardwareClass)) {
            matchingRules.push_back(rule);
        }
    }
    
    // 按优先级排序
    std::sort(matchingRules.begin(), matchingRules.end(),
              [](const PrefetchRule& a, const PrefetchRule& b) {
                  return a.priority > b.priority;
              });
    
    return matchingRules;
}

bool StaticRuleEngine::RuleMatches(const PrefetchRule& rule, 
                                  PrefetchResourceType resourceType, 
                                  SceneType sceneType, 
                                  HardwareClass hardwareClass) {
    
    // 检查资源类型匹配
    if (rule.resourceType != resourceType && 
        rule.resourceType != PrefetchResourceType::Count) {
        return false;
    }
    
    // 检查场景类型匹配
    if (rule.sceneType != sceneType && 
        rule.sceneType != SceneType::Count) {
        return false;
    }
    
    // 检查硬件等级匹配
    if (rule.hardwareClass != hardwareClass && 
        rule.hardwareClass != HardwareClass::Count) {
        return false;
    }
    
    return true;
}

void StaticRuleEngine::InitializeDefaultRules() {
    rules_.clear();
    
    // 几何体预取规则
    PrefetchRule geometryRule = {};
    geometryRule.resourceType = PrefetchResourceType::Geometry;
    geometryRule.sceneType = SceneType::Count; // 适用于所有场景
    geometryRule.hardwareClass = HardwareClass::Count; // 适用于所有硬件
    geometryRule.timeWindow.cpuToGpuFrames = 3;
    geometryRule.timeWindow.renderDataFrames = 2;
    geometryRule.timeWindow.cullingDataFrames = 3;
    geometryRule.timeWindow.streamingDataMs = 500;
    geometryRule.priority = 80;
    geometryRule.confidence = 0.8f;
    geometryRule.memoryBudgetMB = 256;
    geometryRule.enabled = true;
    rules_.push_back(geometryRule);
    
    // 纹理预取规则
    PrefetchRule textureRule = {};
    textureRule.resourceType = PrefetchResourceType::Texture;
    textureRule.sceneType = SceneType::Count;
    textureRule.hardwareClass = HardwareClass::Count;
    textureRule.timeWindow.cpuToGpuFrames = 2;
    textureRule.timeWindow.renderDataFrames = 1;
    textureRule.timeWindow.cullingDataFrames = 2;
    textureRule.timeWindow.streamingDataMs = 200;
    textureRule.priority = 70;
    textureRule.confidence = 0.7f;
    textureRule.memoryBudgetMB = 512;
    textureRule.enabled = true;
    rules_.push_back(textureRule);
    
    // 着色器预取规则
    PrefetchRule shaderRule = {};
    shaderRule.resourceType = PrefetchResourceType::Shader;
    shaderRule.sceneType = SceneType::Count;
    shaderRule.hardwareClass = HardwareClass::Count;
    shaderRule.timeWindow.cpuToGpuFrames = 5;
    shaderRule.timeWindow.renderDataFrames = 3;
    shaderRule.timeWindow.cullingDataFrames = 5;
    shaderRule.timeWindow.streamingDataMs = 100;
    shaderRule.priority = 90;
    shaderRule.confidence = 0.9f;
    shaderRule.memoryBudgetMB = 64;
    shaderRule.enabled = true;
    rules_.push_back(shaderRule);
    
    // 高端硬件专用规则
    PrefetchRule highEndRule = {};
    highEndRule.resourceType = PrefetchResourceType::Count;
    highEndRule.sceneType = SceneType::Count;
    highEndRule.hardwareClass = HardwareClass::High;
    highEndRule.timeWindow.cpuToGpuFrames = 1;
    highEndRule.timeWindow.renderDataFrames = 1;
    highEndRule.timeWindow.cullingDataFrames = 2;
    highEndRule.timeWindow.streamingDataMs = 100;
    highEndRule.priority = 95;
    highEndRule.confidence = 0.95f;
    highEndRule.memoryBudgetMB = 1024;
    highEndRule.enabled = true;
    rules_.push_back(highEndRule);
    
    std::cout << "[INFO] Initialized " << rules_.size() << " default prefetch rules" << std::endl;
}

utl::vector<u64> StaticRuleEngine::GenerateCandidates(
    const FrameData& frameData, 
    const HardwareProfile& profile,
    const AccessPatternAnalyzer& patternAnalyzer) {
    
    utl::vector<u64> candidates;
    
    // 基于可见资源生成候选
    for (u64 resourceId : frameData.visibleResources) {
        candidates.push_back(resourceId);
    }
    
    // 添加热点资源
    auto hotResources = patternAnalyzer.GetHotResources(50);
    for (u64 resourceId : hotResources) {
        if (std::find(candidates.begin(), candidates.end(), resourceId) == candidates.end()) {
            candidates.push_back(resourceId);
        }
    }
    
    // 基于场景类型添加相关资源
    // 这里应该有场景资源图，简化实现
    static const std::unordered_map<SceneType, std::vector<u64>> sceneResources = {
        {SceneType::Indoor, {1001, 1002, 1003}},
        {SceneType::Outdoor, {2001, 2002, 2003}},
        {SceneType::Combat, {3001, 3002, 3003}},
        {SceneType::Menu, {4001, 4002, 4003}}
    };
    
    auto it = sceneResources.find(frameData.currentSceneType);
    if (it != sceneResources.end()) {
        for (u64 resourceId : it->second) {
            if (std::find(candidates.begin(), candidates.end(), resourceId) == candidates.end()) {
                candidates.push_back(resourceId);
            }
        }
    }
    
    return candidates;
}

// ============================================================================
// RHIDeterministicPrefetchManager 实现
// ============================================================================

RHIDeterministicPrefetchManager::RHIDeterministicPrefetchManager(
    const PrefetchConfiguration& config)
    : config_(config)
    , isInitialized_(false)
    , currentMemoryUsage_(0) {
}

bool RHIDeterministicPrefetchManager::Initialize() {
    if (isInitialized_) {
        return true;
    }
    
    // 检查确定性预取是否启用
    if (!config_.enableDeterministic) {
        std::cout << "[WARNING] Deterministic prefetch is disabled" << std::endl;
        return false;
    }
    
    // 初始化硬件分析器
    hardwareAnalyzer_ = std::make_unique<HardwareCapabilityAnalyzer>();
    if (!hardwareAnalyzer_->Initialize()) {
        std::cout << "[ERROR] Failed to initialize hardware capability analyzer" << std::endl;
        return false;
    }
    
    // 初始化访问模式分析器
    patternAnalyzer_ = std::make_unique<AccessPatternAnalyzer>(config_);
    
    // 初始化规则引擎
    ruleEngine_ = std::make_unique<StaticRuleEngine>();
    if (!ruleEngine_->Initialize()) {
        std::cout << "[ERROR] Failed to initialize static rule engine" << std::endl;
        return false;
    }
    
    isInitialized_ = true;
    std::cout << "[INFO] RHIDeterministicPrefetchManager initialized successfully" << std::endl;
    
    return true;
}

void RHIDeterministicPrefetchManager::Update(const FrameData& frameData) {
    if (!isInitialized_) {
        return;
    }
    
    // 更新访问模式分析器
    patternAnalyzer_->UpdateAccessHistory();
    
    // 生成预取决策
    auto decisions = GeneratePrefetchDecisions(frameData);
    
    // 调度异步加载
    ScheduleAsyncLoads(decisions);
    
    // 监控和调整
    MonitorAndAdjust();
}

void RHIDeterministicPrefetchManager::Shutdown() {
    if (!isInitialized_) {
        return;
    }
    
    activeDecisions_.clear();
    hardwareAnalyzer_.reset();
    patternAnalyzer_.reset();
    ruleEngine_.reset();
    
    isInitialized_ = false;
    std::cout << "[INFO] RHIDeterministicPrefetchManager shutdown" << std::endl;
}

utl::vector<PrefetchDecision> RHIDeterministicPrefetchManager::GeneratePrefetchDecisions(
    const FrameData& frameData) {
    
    utl::vector<PrefetchDecision> decisions;
    
    if (!isInitialized_) {
        return decisions;
    }
    
    // 获取硬件配置文件
    const auto& profile = hardwareAnalyzer_->GetProfile();
    
    // 生成候选资源
    auto candidates = ruleEngine_->GenerateCandidates(frameData, profile, *patternAnalyzer_);
    
    // 为每个候选资源生成预取决策
    for (u64 resourceId : candidates) {
        // 评估匹配的规则
        auto matchingRules = ruleEngine_->EvaluateRules(
            PrefetchResourceType::Geometry, // 简化，应该根据资源类型判断
            frameData.currentSceneType,
            profile.hardwareClass
        );
        
        if (matchingRules.empty()) {
            continue;
        }
        
        // 使用最高优先级规则
        const auto& bestRule = matchingRules[0];
        
        // 创建预取决策
        PrefetchDecision decision = {};
        decision.resourceId = resourceId;
        decision.resourceType = PrefetchResourceType::Geometry; // 简化
        decision.targetLOD = 0; // 最高LOD
        decision.frameOffset = bestRule.timeWindow.cpuToGpuFrames;
        decision.priority = bestRule.priority;
        decision.confidence = bestRule.confidence;
        decision.estimatedSize = EstimateResourceSize(decision.resourceType, resourceId);
        decision.shouldPrefetch = HasMemoryBudget(decision.estimatedSize) && 
                                decision.confidence > 0.5f;
        
        if (decision.shouldPrefetch) {
            decisions.push_back(decision);
        }
    }
    
    // 按优先级排序
    std::sort(decisions.begin(), decisions.end(),
              [](const PrefetchDecision& a, const PrefetchDecision& b) {
                  return a.priority > b.priority;
              });
    
    // 更新活跃决策
    activeDecisions_ = decisions;
    
    return decisions;
}

void RHIDeterministicPrefetchManager::ScheduleAsyncLoads(
    const utl::vector<PrefetchDecision>& decisions) {
    
    for (const auto& decision : decisions) {
        if (decision.shouldPrefetch) {
            ScheduleAsyncLoad(decision.resourceId, decision.resourceType, decision.priority);
        }
    }
}

void RHIDeterministicPrefetchManager::MonitorAndAdjust() {
    // 监控内存使用情况
    u64 currentUsage = GetCurrentMemoryUsage();
    currentMemoryUsage_ = currentUsage;
    
    // 如果内存使用超过预算，降低预取优先级
    if (currentUsage > config_.maxMemoryBudgetMB * 1024 * 1024) {
        std::cout << "[WARNING] Memory usage exceeds budget: " 
                  << (currentUsage / (1024 * 1024)) << " MB > " 
                  << config_.maxMemoryBudgetMB << " MB" << std::endl;
        
        // 取消低优先级的预取任务
        for (auto& decision : activeDecisions_) {
            if (decision.priority < 50) {
                decision.shouldPrefetch = false;
            }
        }
    }
}

bool RHIDeterministicPrefetchManager::HasMemoryBudget(u64 requestedSize) {
    u64 currentUsage = GetCurrentMemoryUsage();
    u64 budgetBytes = config_.maxMemoryBudgetMB * 1024 * 1024;
    
    return (currentUsage + requestedSize) <= budgetBytes;
}

f32 RHIDeterministicPrefetchManager::CalculateDeterministicConfidence(
    u64 resourceId, 
    PrefetchResourceType resourceType, 
    const FrameData& frameData) {
    
    // 获取访问频率
    auto freq = patternAnalyzer_->GetAccessFrequency(resourceId);
    
    // 基础置信度基于访问频率
    f32 baseConfidence = std::min(1.0f, freq.accessCount / 100.0f);
    
    // 硬件等级调整
    const auto& profile = hardwareAnalyzer_->GetProfile();
    f32 hardwareMultiplier = 1.0f;
    switch (profile.hardwareClass) {
        case HardwareClass::Ultra: hardwareMultiplier = 1.2f; break;
        case HardwareClass::High: hardwareMultiplier = 1.1f; break;
        case HardwareClass::Medium: hardwareMultiplier = 1.0f; break;
        case HardwareClass::Low: hardwareMultiplier = 0.8f; break;
        case HardwareClass::Count: hardwareMultiplier = 1.0f; break; // 默认值
    }
    
    // 访问模式调整
    f32 patternMultiplier = 1.0f;
    switch (freq.patternType) {
        case AccessType::Sequential: patternMultiplier = 1.3f; break;
        case AccessType::PatternBased: patternMultiplier = 1.2f; break;
        case AccessType::Predictable: patternMultiplier = 1.1f; break;
        case AccessType::Random: patternMultiplier = 0.7f; break;
        case AccessType::Count: patternMultiplier = 1.0f; break; // 默认值
    }
    
    // 综合计算置信度
    f32 confidence = baseConfidence * hardwareMultiplier * patternMultiplier;
    return std::clamp(confidence, 0.0f, 1.0f);
}

bool RHIDeterministicPrefetchManager::ScheduleAsyncLoad(
    u64 resourceId, PrefetchResourceType resourceType, u32 priority) {
    
    // 这里应该与实际的资源加载系统集成
    // 简化实现，只记录日志
    if (config_.enableLogging) {
        std::cout << "[INFO] Scheduling async load: Resource=" << resourceId
                  << ", Type=" << static_cast<int>(resourceType)
                  << ", Priority=" << priority << std::endl;
    }
    
    return true;
}

// 辅助方法实现
u64 RHIDeterministicPrefetchManager::EstimateResourceSize(
    PrefetchResourceType resourceType, u64 resourceId) {
    
    // 简化的资源大小估算
    switch (resourceType) {
        case PrefetchResourceType::Geometry: return 64 * 1024 * 1024; // 64MB
        case PrefetchResourceType::Texture: return 128 * 1024 * 1024;  // 128MB
        case PrefetchResourceType::Shader: return 1 * 1024 * 1024;     // 1MB
        case PrefetchResourceType::Animation: return 32 * 1024 * 1024; // 32MB
        case PrefetchResourceType::Audio: return 16 * 1024 * 1024;     // 16MB
        case PrefetchResourceType::Material: return 4 * 1024 * 1024;    // 4MB
        case PrefetchResourceType::PipelineState: return 256 * 1024;     // 256KB
        default: return 8 * 1024 * 1024; // 默认8MB
    }
}

u64 RHIDeterministicPrefetchManager::GetCurrentMemoryUsage() const {
    // 简化实现，应该从内存管理器获取实际使用量
    return currentMemoryUsage_;
}



// ============================================================================
// 工厂函数实现
// ============================================================================

std::unique_ptr<RHIDeterministicPrefetchManager> CreateDeterministicPrefetchManager(
    const PrefetchConfiguration& config) {
    
    return std::make_unique<RHIDeterministicPrefetchManager>(config);
}



// ============================================================================
// 确定性预取系统接口实现
// ============================================================================

class DeterministicPrefetchSystem : public IDeterministicPrefetchSystem {
private:
    std::unique_ptr<RHIDeterministicPrefetchManager> manager_;
    PrefetchConfiguration config_;
    bool isHealthy_;
    u64 totalPredictions_;
    u64 accuratePredictions_;
    
public:
    explicit DeterministicPrefetchSystem(const PrefetchConfiguration& config)
        : config_(config)
        , isHealthy_(false)
        , totalPredictions_(0)
        , accuratePredictions_(0) {
    }
    
    ~DeterministicPrefetchSystem() override = default;
    
    bool Initialize() override {
        manager_ = std::make_unique<RHIDeterministicPrefetchManager>(config_);
        isHealthy_ = manager_->Initialize();
        return isHealthy_;
    }
    
    void Update(const FrameData& frameData) override {
        if (manager_ && isHealthy_) {
            manager_->Update(frameData);
        }
    }
    
    void Shutdown() override {
        if (manager_) {
            manager_->Shutdown();
            manager_.reset();
        }
        isHealthy_ = false;
    }
    
    void OnResourceAccess(u64 resourceId, PrefetchResourceType type, u32 lodLevel) override {
        if (manager_ && isHealthy_) {
            // 创建访问记录
            AccessRecord record = {};
            record.resourceId = resourceId;
            record.timestamp = GetCurrentTimestamp();
            record.frameNumber = 0; // 应该从frameData获取
            record.resourceType = type;
            record.lodLevel = lodLevel;
            record.accessType = AccessType::Random; // 简化
            record.accessCount = 1;
            
            manager_->GetPatternAnalyzer()->AddAccessRecord(record);
        }
    }
    
    utl::vector<PrefetchDecision> GetPrefetchSuggestions() override {
        if (manager_ && isHealthy_) {
            FrameData frameData = {}; // 创建空的FrameData
            return manager_->GeneratePrefetchDecisions(frameData);
        }
        return {};
    }
    
    void SetSceneType(SceneType sceneType) override {
        // 场景类型应该在FrameData中传递
        // 目前RHIDeterministicPrefetchManager不支持直接设置场景类型
        (void)sceneType; // 避免未使用参数警告
    }
    
    bool IsHealthy() const override {
        return isHealthy_ && manager_ != nullptr;
    }
    
    PerformanceStats GetPerformanceStats() const override {
        f32 accuracy = totalPredictions_ > 0 ? 
            static_cast<f32>(accuratePredictions_) / totalPredictions_ : 0.0f;
        
        return {
            totalPredictions_,
            accuratePredictions_,
            accuracy,
            manager_ ? manager_->GetCurrentMemoryUsage() / (1024 * 1024) : 0,
            0.0f // 平均延迟，需要实际测量
        };
    }
};

std::unique_ptr<IDeterministicPrefetchSystem> IDeterministicPrefetchSystem::Create(
    const PrefetchConfiguration& config) {
    
    return std::make_unique<DeterministicPrefetchSystem>(config);
}

// ============================================================================
// 工具函数实现
// ============================================================================

/**
 * @brief 获取系统内存大小
 * @return 系统内存大小（MB）
 */
u64 GetSystemMemorySize() {
#if defined(__APPLE__)
    // macOS系统内存检测
    int64_t memSize = 0;
    size_t size = sizeof(memSize);
    if (sysctlbyname("hw.memsize", &memSize, &size, nullptr, 0) == 0) {
        return static_cast<u64>(memSize) / (1024 * 1024); // 转换为MB
    }
#elif defined(_WIN32)
    // Windows实现
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<u64>(status.ullTotalPhys) / (1024 * 1024);
    }
#else
    // Linux实现
    FILE* file = fopen("/proc/meminfo", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "MemTotal:", 9) == 0) {
                u64 memKB;
                sscanf(line, "MemTotal: %llu kB", &memKB);
                fclose(file);
                return memKB / 1024; // 转换为MB
            }
        }
        fclose(file);
    }
#endif
    return 8192; // 默认8GB
}

/**
 * @brief 获取当前时间戳
 * @return 时间戳（微秒）
 */
u64 GetCurrentTimestamp() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

} // namespace deterministic_prefetch
} // namespace primal::graphics::rhi