/**
 * @file RHIAdaptiveMemoryPool.cpp
 * @brief RHI自适应内存池系统实现
 * @details 实现智能内存分配、自适应大小调整、使用模式学习和性能优化功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include "RHIAdaptiveMemoryPool.h"
#include "RHIDebug.h"
#include "RHITypes.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cmath>

namespace primal::graphics::rhi {

// === 常量定义 ===

namespace {
    constexpr u64 MICROSECONDS_PER_SECOND = 1000000ULL;
    constexpr u64 DEFAULT_TIMESTAMP = 0;
    constexpr f32 CONFIDENCE_EPSILON = 0.001f;
    constexpr u32 MAX_USAGE_WINDOWS = 100;
    constexpr u32 HOTSPOT_PREALLOCATION_COUNT = 50;
    constexpr f32 MEMORY_EFFICIENCY_THRESHOLD = 0.85f;
}

// === 构造函数和析构函数 ===

RHIAdaptiveMemoryPool::RHIAdaptiveMemoryPool(RHIDeviceBase& device, const MemoryPoolDesc& desc, 
                                            const AdaptiveConfig& config)
    : RHIMemoryPool(device, desc)
    , config_(config)
    , metrics_()
    , adaptiveAnalysisRunning_(false)
    , analysisStartTime_(DEFAULT_TIMESTAMP)
    , lastPatternAnalysis_(DEFAULT_TIMESTAMP)
    , nextBlockHandle_(1)
    , totalAllocationTime_(0)
    , totalDeallocationTime_(0)
    , lastResizeTime_(DEFAULT_TIMESTAMP)
    , lastDefragmentationTime_(DEFAULT_TIMESTAMP) {
    
    // 预分配容器空间
    activeAllocations_.reserve(config_.maxPoolSize / 1024); // 估算活跃分配数量
    hotspotBlocks_.reserve(HOTSPOT_PREALLOCATION_COUNT);
    preallocatedBlocks_.reserve(HOTSPOT_PREALLOCATION_COUNT);
    memoryBlocks_.reserve(1024);
    freeBlocks_.reserve(256);
    
    // 创建初始的空闲块（整个池）
    MemoryBlock initialBlock;
    initialBlock.offset = 0;
    initialBlock.size = desc.poolSize;
    initialBlock.alignment = desc.alignment;
    initialBlock.state = MemoryBlockState::Free;
    initialBlock.usage = GPUMemoryUsage::Unknown;
    
    memoryBlocks_.push_back(initialBlock);
    freeBlocks_.push_back(0); // 第一个块的索引
    
    logAdaptiveEvent("RHIAdaptiveMemoryPool创建", 
                    ("池大小: " + std::to_string(desc.poolSize) + " 字节").c_str());
}

RHIAdaptiveMemoryPool::~RHIAdaptiveMemoryPool() {
    StopAdaptiveAnalysis();
    logAdaptiveEvent("RHIAdaptiveMemoryPool销毁");
}

// === RHIMemoryPool接口实现 ===

bool RHIAdaptiveMemoryPool::Initialize() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (initialized_) {
        return true;
    }
    
    // 初始化自适应系统
    analysisStartTime_ = getCurrentTimestamp();
    lastPatternAnalysis_ = analysisStartTime_;
    lastResizeTime_ = analysisStartTime_;
    lastDefragmentationTime_ = analysisStartTime_;
    
    // 预分配热点内存块
    PreallocateHotspots(HOTSPOT_PREALLOCATION_COUNT);
    
    initialized_ = true;
    
    return true;
}

u32 RHIAdaptiveMemoryPool::Allocate(u64 size, u64 alignment, GPUMemoryUsage usage) {
    const u64 startTime = getCurrentTimestamp();
    
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    
    // 尝试从预分配的热点池中分配
    u32 blockHandle = findFreePreallocatedBlock(size, alignment);
    if (blockHandle != u32_invalid_id) {
        // 从预分配池中成功分配
        removeFromPreallocatedPool(blockHandle);
    } else {
        // 从主内存池中分配
        blockHandle = allocateFromMemoryPool(size, alignment, usage);
        if (blockHandle == u32_invalid_id) {
            // 分配失败，尝试自适应调整后重试
            if (adaptiveAnalysisRunning_ && shouldResizePool()) {
                PerformAdaptiveAdjustment();
                blockHandle = allocateFromMemoryPool(size, alignment, usage);
            }
        }
    }
    
    if (blockHandle != u32_invalid_id) {
        // 记录分配信息
        recordAllocation(blockHandle, size, alignment, usage);
        activeAllocations_.push_back(blockHandle);
        
        // 更新热点状态
        if (adaptiveAnalysisRunning_) {
            updateHotspotStatus(blockHandle);
        }
        
        // 更新性能指标
        const u64 allocationTime = getCurrentTimestamp() - startTime;
        updateAllocationMetrics(allocationTime);
        
        // 检查是否需要自适应调整
        if (adaptiveAnalysisRunning_) {
            updateUsageWindow();
            if (shouldResizePool() || shouldDefragment()) {
                // 异步触发调整（避免阻塞分配）
                std::thread([this]() {
                    PerformAdaptiveAdjustment();
                }).detach();
            }
        }
    } else {
        // 分配失败
        std::lock_guard<std::mutex> metricsLock(metricsMutex_);
        metrics_.failedAllocationCount++;
    }
    
    return blockHandle;
}

bool RHIAdaptiveMemoryPool::Deallocate(u32 blockHandle) {
    const u64 startTime = getCurrentTimestamp();
    
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    
    if (!IsValidBlock(blockHandle)) {
        return false;
    }
    
    // 记录释放信息
    recordDeallocation(blockHandle);
    
    // 从活跃分配列表中移除
    auto it = std::find(activeAllocations_.begin(), activeAllocations_.end(), blockHandle);
    if (it != activeAllocations_.end()) {
        activeAllocations_.erase(it);
    }
    
    // 检查是否为热点内存块
    bool isHotspotBlock = isHotspot(blockHandle);
    
    deallocateFromMemoryPool(blockHandle);
    
    // 如果是热点块，重新加入预分配池
    if (isHotspotBlock && adaptiveAnalysisRunning_) {
        addToPreallocatedPool(blockHandle);
    }
    
    // 更新性能指标
    const u64 deallocationTime = getCurrentTimestamp() - startTime;
    updateDeallocationMetrics(deallocationTime);
    
    // 更新内存效率
    updateMemoryEfficiency();
    
    return true;
}

u32 RHIAdaptiveMemoryPool::Reallocate(u32 blockHandle, u64 newSize, u64 newAlignment) {
    if (!IsValidBlock(blockHandle)) {
        return u32_invalid_id;
    }
    
    // 获取当前内存块信息
    MemoryBlock currentBlock = GetMemoryBlock(blockHandle);
    
    // 如果新大小和当前大小相同，直接返回原句柄
    if (newSize == currentBlock.size && (newAlignment == 0 || newAlignment == currentBlock.alignment)) {
        return blockHandle;
    }
    
    // 分配新的内存块
    u32 newBlockHandle = Allocate(newSize, newAlignment, currentBlock.usage);
    if (newBlockHandle == u32_invalid_id) {
        return u32_invalid_id;
    }
    
    // 复制数据（这里简化处理，实际需要实现数据复制）
    // copyBlockData(blockHandle, newBlockHandle, std::min(newSize, currentBlock.size));
    
    // 释放原内存块
    Deallocate(blockHandle);
    
    return newBlockHandle;
}

bool RHIAdaptiveMemoryPool::Defragment() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (!initialized_) {
        return false;
    }
    
    // TODO: 实现安全的碎片整理
    // 当前实现使用了 std::sort，这会破坏 handleToBlockIndex_ 映射（因为它是基于索引的）。
    // 在找到安全的重建映射方法之前，暂时禁用碎片整理。
    return false;

    /*
    const u64 startTime = getCurrentTimestamp();
    
    // 简单的碎片整理实现：合并相邻的空闲块
    std::sort(memoryBlocks_.begin(), memoryBlocks_.end(), 
              [](const MemoryBlock& a, const MemoryBlock& b) {
                  return a.offset < b.offset;
              });
    
    // 重建空闲块列表
    freeBlocks_.clear();
    bool success = true;
    
    for (size_t i = 0; i < memoryBlocks_.size(); ++i) {
        MemoryBlock& block = memoryBlocks_[i];
        if (block.state == MemoryBlockState::Free) {
            // 尝试与下一个空闲块合并
            if (i + 1 < memoryBlocks_.size() && 
                memoryBlocks_[i + 1].state == MemoryBlockState::Free &&
                block.offset + block.size == memoryBlocks_[i + 1].offset) {
                
                block.size += memoryBlocks_[i + 1].size;
                memoryBlocks_.erase(memoryBlocks_.begin() + i + 1);
                
                // 更新映射：所有位于删除索引之后的块，其索引都需要减1
                u32 erasedIndex = static_cast<u32>(i + 1);
                for (auto& pair : handleToBlockIndex_) {
                    if (pair.second > erasedIndex) {
                        pair.second--;
                    }
                }
                
                --i; // 重新检查当前块
            }
            freeBlocks_.push_back(static_cast<u32>(i));
        }
    }
    
    // 更新碎片整理指标
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    metrics_.defragmentationCount++;
    const u64 defragTime = getCurrentTimestamp() - startTime;
    metrics_.totalDefragmentationTime += defragTime;
    lastDefragmentationTime_ = getCurrentTimestamp();
    
    // 更新预分配热点块
    if (adaptiveAnalysisRunning_) {
        updateHotspotPreallocation();
    }
    
    return success;
    */
}

bool RHIAdaptiveMemoryPool::Clear() {
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    std::lock_guard<std::mutex> adaptiveLock(adaptiveMutex_);
    
    if (!initialized_) {
        return false;
    }
    
    // 重置内存块状态
    for (auto& block : memoryBlocks_) {
        block.state = MemoryBlockState::Free;
        block.usage = GPUMemoryUsage::Unknown;
    }
    
    // 重建空闲块列表
    freeBlocks_.clear();
    for (size_t i = 0; i < memoryBlocks_.size(); ++i) {
        freeBlocks_.push_back(static_cast<u32>(i));
    }
    
    // 清空所有分配记录
    allocationRecords_.clear();
    activeAllocations_.clear();
    handleToBlockIndex_.clear();
    accessTimestamps_.clear();
    
    // 清空热点和预分配块
    hotspotBlocks_.clear();
    preallocatedBlocks_.clear();
    
    // 清空使用窗口
    usageWindows_.clear();
    
    // 重置性能指标
    totalAllocationTime_ = 0;
    totalDeallocationTime_ = 0;
    
    // 重置自适应指标
    ResetAdaptiveMetrics();
    
    return true;
}

MemoryBlock RHIAdaptiveMemoryPool::GetMemoryBlock(u32 blockHandle) const {
    auto it = handleToBlockIndex_.find(blockHandle);
    if (it != handleToBlockIndex_.end()) {
        u32 blockIndex = it->second;
        if (blockIndex < memoryBlocks_.size()) {
            return memoryBlocks_[blockIndex];
        }
    }
    return MemoryBlock{};
}

bool RHIAdaptiveMemoryPool::IsValidBlock(u32 blockHandle) const {
    return handleToBlockIndex_.find(blockHandle) != handleToBlockIndex_.end();
}

bool RHIAdaptiveMemoryPool::Validate() const {
    return validateAllocationRecords() && validateHotspotConsistency();
}

void RHIAdaptiveMemoryPool::destroyImpl() {
    StopAdaptiveAnalysis();
    
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    std::lock_guard<std::mutex> adaptiveLock(adaptiveMutex_);
    
    // 清理所有自适应数据
    allocationRecords_.clear();
    activeAllocations_.clear();
    handleToBlockIndex_.clear();
    accessTimestamps_.clear();
    hotspotBlocks_.clear();
    preallocatedBlocks_.clear();
    usageWindows_.clear();
    
}

void RHIAdaptiveMemoryPool::updateStats() {
    // 更新自适应指标
    updateMemoryEfficiency();
}

// === 自适应功能接口实现 ===

bool RHIAdaptiveMemoryPool::StartAdaptiveAnalysis() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (adaptiveAnalysisRunning_) {
        return true;
    }
    
    if (!initialized_) {
        return false;
    }
    
    // 重置分析状态
    analysisStartTime_ = getCurrentTimestamp();
    lastPatternAnalysis_ = analysisStartTime_;
    usageWindows_.clear();
    
    // 重置指标
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    metrics_ = AdaptiveMetrics();
    metrics_.detectedPattern = MemoryUsagePattern::Unknown;
    
    adaptiveAnalysisRunning_ = true;
    
    return true;
}

void RHIAdaptiveMemoryPool::StopAdaptiveAnalysis() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (!adaptiveAnalysisRunning_) {
        return;
    }
    
    adaptiveAnalysisRunning_ = false;
    
    // 生成最终分析报告
    PrintAdaptiveReport();
}

MemoryUsagePattern RHIAdaptiveMemoryPool::AnalyzeUsagePattern() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (!adaptiveAnalysisRunning_) {
        return MemoryUsagePattern::Unknown;
    }
    
    // 更新当前使用窗口
    updateUsageWindow();
    
    // 检测使用模式
    MemoryUsagePattern detectedPattern = detectUsagePattern(usageWindows_);
    f32 confidence = calculatePatternConfidence(detectedPattern, usageWindows_);
    
    // 更新指标
    {
        std::lock_guard<std::mutex> metricsLock(metricsMutex_);
        if (confidence > metrics_.patternConfidence) {
            metrics_.detectedPattern = detectedPattern;
            metrics_.patternConfidence = confidence;
            metrics_.patternStartTime = getCurrentTimestamp();
        }
    }
    
    lastPatternAnalysis_ = getCurrentTimestamp();
    
    return detectedPattern;
}

bool RHIAdaptiveMemoryPool::PerformAdaptiveAdjustment() {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    if (!adaptiveAnalysisRunning_) {
        return false;
    }
    
    const u64 currentTime = getCurrentTimestamp();
    
    // 检查冷却时间
    if (currentTime - lastResizeTime_ < config_.resizeCooldown * MICROSECONDS_PER_SECOND) {
        return false;
    }
    
    bool adjustmentPerformed = false;
    
    // 检查是否需要调整池大小
    if (shouldResizePool()) {
        u64 optimalSize = calculateOptimalPoolSize();
        if (optimalSize != desc_.poolSize) {
            if (resizePool(optimalSize)) {
                adjustmentPerformed = true;
                lastResizeTime_ = currentTime;
                
                std::lock_guard<std::mutex> metricsLock(metricsMutex_);
                metrics_.resizeCount++;
            }
        }
    }
    
    // 检查是否需要碎片整理
    if (shouldDefragment()) {
        if (Defragment()) {
            adjustmentPerformed = true;
            lastDefragmentationTime_ = currentTime;
        }
    }
    
    // 更新热点预分配
    if (adjustmentPerformed) {
        updateHotspotPreallocation();
    }
    
    return adjustmentPerformed;
}

u32 RHIAdaptiveMemoryPool::AnalyzeHotspots() {
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    
    if (!adaptiveAnalysisRunning_) {
        return 0;
    }
    
    hotspotBlocks_.clear();
    
    for (const auto& pair : allocationRecords_) {
        u32 blockHandle = pair.first;
        const AllocationRecord& record = pair.second;
        
        if (record.deallocationTimestamp == DEFAULT_TIMESTAMP) { // 仍然活跃的块
            MemoryHotspotLevel level = calculateHotspotLevel(record);
            if (level >= MemoryHotspotLevel::Hot) {
                hotspotBlocks_.push_back(blockHandle);
            }
        }
    }
    
    // 更新热点指标
    {
        std::lock_guard<std::mutex> metricsLock(metricsMutex_);
        metrics_.totalHotspots = static_cast<u32>(hotspotBlocks_.size());
        metrics_.hotspotMemorySize = 0; // 需要计算热点内存总大小
        
        for (u32 blockHandle : hotspotBlocks_) {
            MemoryBlock block = GetMemoryBlock(blockHandle);
            metrics_.hotspotMemorySize += block.size;
        }
        
        if (stats_.allocatedSize > 0) {
            metrics_.hotspotHitRatio = static_cast<f32>(metrics_.hotspotMemorySize) / 
                                       static_cast<f32>(stats_.allocatedSize);
        }
    }
    
    return static_cast<u32>(hotspotBlocks_.size());
}

bool RHIAdaptiveMemoryPool::PreallocateHotspots(u32 hotspotCount) {
    std::lock_guard<std::mutex> allocLock(allocationMutex_);
    
    if (!initialized_) {
        return false;
    }
    
    // 计算合适的预分配块大小（基于历史数据）
    u64 averageBlockSize = 64 * 1024; // 默认64KB
    if (!allocationRecords_.empty()) {
        u64 totalSize = 0;
        u32 count = 0;
        for (const auto& pair : allocationRecords_) {
            if (pair.second.deallocationTimestamp == DEFAULT_TIMESTAMP) {
                totalSize += pair.second.size;
                count++;
            }
        }
        if (count > 0) {
            averageBlockSize = totalSize / count;
        }
    }
    
    // 预分配热点块
    for (u32 i = 0; i < hotspotCount; ++i) {
        u32 blockHandle = allocateFromMemoryPool(averageBlockSize, 256, GPUMemoryUsage::Dynamic);
        if (blockHandle != u32_invalid_id) {
            preallocatedBlocks_.push_back(blockHandle);
            
            // 初始化分配记录
            AllocationRecord record;
            record.timestamp = getCurrentTimestamp();
            record.size = static_cast<u32>(averageBlockSize);
            record.alignment = 256;
            record.usage = GPUMemoryUsage::Dynamic;
            record.hotspotLevel = MemoryHotspotLevel::Warm; // 预分配块设为温点
            
            allocationRecords_[blockHandle] = record;
        } else {
            break;
        }
    }
    
    return !preallocatedBlocks_.empty();
}

// === 访问器方法实现 ===

void RHIAdaptiveMemoryPool::SetAdaptiveConfig(const AdaptiveConfig& config) {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    config_ = config;
}

// === 调试和诊断方法实现 ===

void RHIAdaptiveMemoryPool::PrintAdaptiveReport() const {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    printf("=== RHI自适应内存池报告 ===\n");
    printf("内存池名称: %s\n", desc_.name);
    printf("自适应分析状态: %s\n", adaptiveAnalysisRunning_ ? "运行中" : "已停止");
    printf("运行时间: %.2f 小时\n", 
           (getCurrentTimestamp() - analysisStartTime_) / (3600.0f * MICROSECONDS_PER_SECOND));
    
    printf("\n--- 使用模式分析 ---\n");
    printf("检测模式: %d\n", static_cast<int>(metrics_.detectedPattern));
    printf("模式置信度: %.2f%%\n", metrics_.patternConfidence * 100.0f);
    printf("模式持续时间: %.2f 分钟\n",
           (getCurrentTimestamp() - metrics_.patternStartTime) / (60.0f * MICROSECONDS_PER_SECOND));
    
    printf("\n--- 自适应调整 ---\n");
    printf("调整次数: %u\n", metrics_.resizeCount);
    printf("总调整时间: %.2f ms\n", metrics_.totalResizeTime / 1000.0f);
    printf("碎片整理次数: %u\n", metrics_.defragmentationCount);
    printf("总整理时间: %.2f ms\n", metrics_.totalDefragmentationTime / 1000.0f);
    
    printf("\n--- 热点统计 ---\n");
    printf("热点块数量: %u\n", metrics_.totalHotspots);
    printf("热点内存大小: %.2f MB\n", metrics_.hotspotMemorySize / (1024.0f * 1024.0f));
    printf("热点命中率: %.2f%%\n", metrics_.hotspotHitRatio * 100.0f);
    
    printf("\n--- 性能指标 ---\n");
    printf("平均分配时间: %.2f μs\n", metrics_.averageAllocationTime);
    printf("平均释放时间: %.2f μs\n", metrics_.averageDeallocationTime);
    printf("内存效率: %.2f%%\n", metrics_.memoryEfficiency * 100.0f);
    printf("分配成功率: %.2f%%\n", metrics_.allocationSuccessRate * 100.0f);
    printf("失败分配次数: %u\n", metrics_.failedAllocationCount);
    
    printf("============================\n");
}

std::string RHIAdaptiveMemoryPool::GenerateAdaptiveReport() const {
    std::ostringstream report;
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    report << "=== RHI自适应内存池详细报告 ===\n";
    report << "生成时间: " << getCurrentTimestamp() << "\n";
    report << "内存池名称: " << desc_.name << "\n";
    report << "基础池大小: " << desc_.poolSize << " 字节\n";
    report << "自适应分析状态: " << (adaptiveAnalysisRunning_ ? "运行中" : "已停止") << "\n";
    
    // 添加更多详细信息...
    
    report << "================================\n";
    return report.str();
}

bool RHIAdaptiveMemoryPool::ExportUsagePatternData(const char* filename) const {
    std::lock_guard<std::mutex> lock(adaptiveMutex_);
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }
    
    file << "Timestamp,AllocationCount,DeallocationCount,PeakUsage,AverageUsage,FragmentationRatio\n";
    
    for (const auto& window : usageWindows_) {
        file << window.startTime << ","
             << window.allocationCount << ","
             << window.deallocationCount << ","
             << window.peakUsage << ","
             << window.averageUsage << ","
             << window.fragmentationRatio << "\n";
    }
    
    file.close();
    return true;
}

void RHIAdaptiveMemoryPool::ResetAdaptiveMetrics() {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    metrics_ = AdaptiveMetrics();
}

// === 私有辅助方法实现 ===

MemoryUsagePattern RHIAdaptiveMemoryPool::detectUsagePattern(const std::deque<UsageWindow>& windows) {
    if (windows.size() < config_.patternDetectionThreshold) {
        return MemoryUsagePattern::Unknown;
    }
    
    // 分析最近N个窗口的特征
    std::vector<f32> allocationRates;
    std::vector<f32> usageVariations;
    
    for (size_t i = 1; i < windows.size(); ++i) {
        const UsageWindow& current = windows[i];
        const UsageWindow& previous = windows[i-1];
        
        // 计算分配率变化
        f32 allocationRate = static_cast<f32>(current.allocationCount - current.deallocationCount);
        allocationRates.push_back(allocationRate);
        
        // 计算使用量变化
        double usageDiff = static_cast<double>(current.averageUsage) - static_cast<double>(previous.averageUsage);
        double timeDiff = static_cast<double>(current.endTime - previous.endTime);
        if (timeDiff > 0) {
            usageVariations.push_back(static_cast<f32>(usageDiff / timeDiff));
        }
    }
    
    // 基于统计特征判断模式
    if (allocationRates.empty()) {
        return MemoryUsagePattern::Steady;
    }
    
    // 计算变异系数
    f32 meanRate = 0.0f;
    for (f32 rate : allocationRates) {
        meanRate += rate;
    }
    meanRate /= allocationRates.size();
    
    f32 variance = 0.0f;
    for (f32 rate : allocationRates) {
        f32 diff = rate - meanRate;
        variance += diff * diff;
    }
    variance /= allocationRates.size();
    
    f32 stdDev = std::sqrt(variance);
    f32 coefficientOfVariation = (meanRate != 0.0f) ? (stdDev / std::abs(meanRate)) : 0.0f;
    
    // 模式判断逻辑
    if (coefficientOfVariation > 2.0f) {
        return MemoryUsagePattern::Burst; // 高变异：突发模式
    } else if (coefficientOfVariation < 0.3f) {
        if (std::abs(meanRate) < 1.0f) {
            return MemoryUsagePattern::Steady; // 低变异且接近零：稳定模式
        } else if (meanRate > 0.0f) {
            return MemoryUsagePattern::Growing; // 持续正增长：增长模式
        } else {
            return MemoryUsagePattern::Shrinking; // 持续负增长：收缩模式
        }
    } else {
        // 检查周期性特征
        if (allocationRates.size() >= 10) {
            // 简化的周期性检测：检查是否存在规律波动
            u32 signChanges = 0;
            for (size_t i = 1; i < allocationRates.size(); ++i) {
                if ((allocationRates[i] > 0) != (allocationRates[i-1] > 0)) {
                    signChanges++;
                }
            }
            
            f32 signChangeRatio = static_cast<f32>(signChanges) / (allocationRates.size() - 1);
            if (signChangeRatio > 0.4f) {
                return MemoryUsagePattern::Periodic; // 频繁符号变化：周期模式
            }
        }
        
        return MemoryUsagePattern::Unknown; // 无法明确分类
    }
}

f32 RHIAdaptiveMemoryPool::calculatePatternConfidence(MemoryUsagePattern pattern, const std::deque<UsageWindow>& windows) {
    if (windows.size() < config_.patternDetectionThreshold) {
        return 0.0f;
    }
    
    // 基于模式一致性和数据量计算置信度
    f32 dataQuality = std::min(1.0f, static_cast<f32>(windows.size()) / 20.0f); // 最多20个窗口
    
    f32 patternConsistency = 0.8f; // 基础一致性
    if (pattern != MemoryUsagePattern::Unknown) {
        // 计算模式的一致性分数
        std::vector<MemoryUsagePattern> recentPatterns;
        for (size_t i = windows.size() / 2; i < windows.size(); ++i) {
            // 简化处理：基于窗口特征判断模式
            // 实际应该复用detectUsagePattern的逻辑
            recentPatterns.push_back(pattern); // 简化
        }
        
        u32 consistentCount = 0;
        for (MemoryUsagePattern p : recentPatterns) {
            if (p == pattern) {
                consistentCount++;
            }
        }
        
        patternConsistency = static_cast<f32>(consistentCount) / recentPatterns.size();
    }
    
    return dataQuality * patternConsistency;
}

void RHIAdaptiveMemoryPool::updateUsageWindow() {
    const u64 currentTime = getCurrentTimestamp();
    const u64 windowSize = config_.patternAnalysisWindowSize * MICROSECONDS_PER_SECOND;
    
    // 如果当前窗口不存在或已过期，创建新窗口
    if (usageWindows_.empty() || 
        (currentTime - usageWindows_.back().endTime) > windowSize) {
        
        UsageWindow newWindow;
        newWindow.startTime = currentTime;
        newWindow.endTime = currentTime + windowSize;
        newWindow.allocationCount = 0;
        newWindow.deallocationCount = 0;
        newWindow.peakUsage = stats_.allocatedSize;
        newWindow.averageUsage = stats_.allocatedSize;
        newWindow.fragmentationRatio = stats_.fragmentationRatio;
        newWindow.hotspotCount = static_cast<u32>(hotspotBlocks_.size());
        
        usageWindows_.push_back(newWindow);
        
        // 限制窗口数量
        while (usageWindows_.size() > MAX_USAGE_WINDOWS) {
            usageWindows_.pop_front();
        }
    } else {
        // 更新当前窗口
        UsageWindow& currentWindow = usageWindows_.back();
        currentWindow.endTime = currentTime;
        currentWindow.peakUsage = std::max(currentWindow.peakUsage, stats_.allocatedSize);
        currentWindow.averageUsage = stats_.allocatedSize;
        currentWindow.fragmentationRatio = stats_.fragmentationRatio;
        currentWindow.hotspotCount = static_cast<u32>(hotspotBlocks_.size());
    }
}

void RHIAdaptiveMemoryPool::recordAllocation(u32 blockHandle, u64 size, u64 alignment, GPUMemoryUsage usage) {
    AllocationRecord record;
    record.timestamp = getCurrentTimestamp();
    record.deallocationTimestamp = DEFAULT_TIMESTAMP;
    record.size = static_cast<u32>(size);
    record.alignment = static_cast<u32>(alignment);
    record.usage = usage;
    record.accessCount = 1;
    record.totalAccessTime = 0;
    record.hotspotLevel = MemoryHotspotLevel::Cold;
    
    allocationRecords_[blockHandle] = record;
    accessTimestamps_[blockHandle] = record.timestamp;
}

void RHIAdaptiveMemoryPool::recordDeallocation(u32 blockHandle) {
    auto it = allocationRecords_.find(blockHandle);
    if (it != allocationRecords_.end()) {
        it->second.deallocationTimestamp = getCurrentTimestamp();
    }
    
    auto timestampIt = accessTimestamps_.find(blockHandle);
    if (timestampIt != accessTimestamps_.end()) {
        accessTimestamps_.erase(timestampIt);
    }
}

void RHIAdaptiveMemoryPool::updateHotspotStatus(u32 blockHandle) {
    auto it = allocationRecords_.find(blockHandle);
    if (it == allocationRecords_.end()) {
        return;
    }
    
    AllocationRecord& record = it->second;
    
    // 更新访问信息
    u64 currentTime = getCurrentTimestamp();
    record.accessCount++;
    record.totalAccessTime += (currentTime - record.timestamp);
    record.timestamp = currentTime;
    
    // 重新计算热点级别
    MemoryHotspotLevel newLevel = calculateHotspotLevel(record);
    if (newLevel != record.hotspotLevel) {
        record.hotspotLevel = newLevel;
        
        // 更新热点块列表
        if (newLevel >= MemoryHotspotLevel::Hot) {
            if (std::find(hotspotBlocks_.begin(), hotspotBlocks_.end(), blockHandle) == hotspotBlocks_.end()) {
                hotspotBlocks_.push_back(blockHandle);
            }
        } else {
            auto hotIt = std::find(hotspotBlocks_.begin(), hotspotBlocks_.end(), blockHandle);
            if (hotIt != hotspotBlocks_.end()) {
                hotspotBlocks_.erase(hotIt);
            }
        }
    }
    
    accessTimestamps_[blockHandle] = currentTime;
}

MemoryHotspotLevel RHIAdaptiveMemoryPool::calculateHotspotLevel(const AllocationRecord& record) {
    if (record.accessCount == 0) {
        return MemoryHotspotLevel::Cold;
    }
    
    u64 currentTime = getCurrentTimestamp();
    u64 age = currentTime - record.timestamp;
    
    // 计算访问频率（每秒访问次数）
    double ageSeconds = static_cast<double>(age) / MICROSECONDS_PER_SECOND;
    double accessFrequency = (ageSeconds > 0.0) ? (static_cast<double>(record.accessCount) / ageSeconds) : 0.0;
    
    // 计算平均访问间隔
    double avgAccessInterval = (record.accessCount > 1) ? 
        (static_cast<double>(record.totalAccessTime) / (record.accessCount - 1)) : 0.0;
    
    // 热点级别判断
    if (accessFrequency > 10.0 || avgAccessInterval < 0.1 * MICROSECONDS_PER_SECOND) {
        return MemoryHotspotLevel::Critical; // 极高频访问
    } else if (accessFrequency > 5.0 || avgAccessInterval < 0.5 * MICROSECONDS_PER_SECOND) {
        return MemoryHotspotLevel::Hot; // 高频访问
    } else if (accessFrequency > 1.0 || avgAccessInterval < 2.0 * MICROSECONDS_PER_SECOND) {
        return MemoryHotspotLevel::Warm; // 中频访问
    } else {
        return MemoryHotspotLevel::Cold; // 低频访问
    }
}

bool RHIAdaptiveMemoryPool::isHotspot(u32 blockHandle) const {
    auto it = allocationRecords_.find(blockHandle);
    return (it != allocationRecords_.end() && 
            it->second.hotspotLevel >= MemoryHotspotLevel::Hot);
}

bool RHIAdaptiveMemoryPool::shouldResizePool() {
    // 基于使用率和模式决定是否调整
    f32 usageRatio = GetUsageRatio();
    
    // 高使用率且为增长模式，需要扩容
    if (usageRatio > 0.9f && metrics_.detectedPattern == MemoryUsagePattern::Growing) {
        return true;
    }
    
    // 低使用率且为收缩模式，需要缩容
    if (usageRatio < 0.3f && metrics_.detectedPattern == MemoryUsagePattern::Shrinking) {
        return true;
    }
    
    // 碎片化严重时考虑扩容
    if (stats_.fragmentationRatio > config_.defragmentationThreshold && usageRatio > 0.8f) {
        return true;
    }
    
    return false;
}

bool RHIAdaptiveMemoryPool::shouldDefragment() {
    const u64 currentTime = getCurrentTimestamp();
    
    // 检查碎片整理间隔
    if (currentTime - lastDefragmentationTime_ < config_.defragmentationInterval * MICROSECONDS_PER_SECOND) {
        return false;
    }
    
    // 碎片化比例超过阈值
    if (stats_.fragmentationRatio > config_.defragmentationThreshold) {
        return true;
    }
    
    // 分配失败率高时触发整理
    f32 failureRate = (metrics_.allocationCount > 0) ? 
        (static_cast<f32>(metrics_.failedAllocationCount) / metrics_.allocationCount) : 0.0f;
    if (failureRate > 0.05f) { // 失败率超过5%
        return true;
    }
    
    return false;
}

u64 RHIAdaptiveMemoryPool::calculateOptimalPoolSize() {
    // 基于历史数据和当前模式计算最优大小
    u64 currentSize = desc_.poolSize;
    u64 peakUsage = stats_.peakUsage;
    f32 usageRatio = GetUsageRatio();
    
    // 基础大小：峰值使用量 + 20%缓冲
    u64 baseSize = static_cast<u64>(peakUsage * 1.2f);
    
    // 根据使用模式调整
    switch (metrics_.detectedPattern) {
        case MemoryUsagePattern::Burst:
            // 突发模式需要更大的缓冲
            baseSize = static_cast<u64>(baseSize * 1.5f);
            break;
            
        case MemoryUsagePattern::Growing:
            // 增长模式需要预留空间
            baseSize = static_cast<u64>(baseSize * config_.growthFactor);
            break;
            
        case MemoryUsagePattern::Shrinking:
            // 收缩模式可以减少空间
            baseSize = static_cast<u64>(baseSize * config_.shrinkFactor);
            break;
            
        case MemoryUsagePattern::Periodic:
            // 周期模式需要考虑峰值
            baseSize = std::max(baseSize, peakUsage);
            break;
            
        case MemoryUsagePattern::Steady:
        default:
            // 稳定模式使用基础大小
            break;
    }
    
    // 限制在配置范围内
    baseSize = std::max(baseSize, static_cast<u64>(config_.minPoolSize));
    baseSize = std::min(baseSize, static_cast<u64>(config_.maxPoolSize));
    
    return baseSize;
}

bool RHIAdaptiveMemoryPool::resizePool(u64 newSize) {
    // 这里需要调用具体的内存池实现来调整大小
    // 由于是基类，这里只是一个框架
    
    if (newSize == desc_.poolSize) {
        return true; // 无需调整
    }
    
    // 更新描述符
    desc_.poolSize = newSize;
    
    // 这里应该调用具体实现的重大小逻辑
    // 实际实现需要考虑数据迁移等问题
    
    return true;
}

void RHIAdaptiveMemoryPool::updateHotspotPreallocation() {
    // 分析当前热点情况，调整预分配策略
    u32 currentHotspotCount = static_cast<u32>(hotspotBlocks_.size());
    u32 targetPreallocationCount = std::min(HOTSPOT_PREALLOCATION_COUNT, 
                                           static_cast<u32>(currentHotspotCount * 1.5f));
    
    // 如果预分配块不足，增加预分配
    if (preallocatedBlocks_.size() < targetPreallocationCount) {
        PreallocateHotspots(targetPreallocationCount - static_cast<u32>(preallocatedBlocks_.size()));
    }
    // 如果预分配块过多，释放一些
    else if (preallocatedBlocks_.size() > targetPreallocationCount * 2) {
        u32 releaseCount = static_cast<u32>(preallocatedBlocks_.size()) - targetPreallocationCount;
        for (u32 i = 0; i < releaseCount && !preallocatedBlocks_.empty(); ++i) {
            u32 blockHandle = preallocatedBlocks_.back();
            preallocatedBlocks_.erase_unordered(preallocatedBlocks_.size() - 1);
            deallocateFromMemoryPool(blockHandle);
            
            auto recordIt = allocationRecords_.find(blockHandle);
            if (recordIt != allocationRecords_.end()) {
                allocationRecords_.erase(recordIt);
            }
        }
    }
}

void RHIAdaptiveMemoryPool::updateAllocationMetrics(u64 allocationTime) {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    metrics_.allocationCount++;
    totalAllocationTime_ += allocationTime;
    metrics_.averageAllocationTime = static_cast<f32>(totalAllocationTime_) / metrics_.allocationCount;
    
    // 更新分配成功率
    u64 totalAttempts = metrics_.allocationCount + metrics_.failedAllocationCount;
    if (totalAttempts > 0) {
        metrics_.allocationSuccessRate = static_cast<f32>(metrics_.allocationCount) / totalAttempts;
    }
}

void RHIAdaptiveMemoryPool::updateDeallocationMetrics(u64 deallocationTime) {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    metrics_.deallocationCount++;
    totalDeallocationTime_ += deallocationTime;
    metrics_.averageDeallocationTime = static_cast<f32>(totalDeallocationTime_) / metrics_.deallocationCount;
}

void RHIAdaptiveMemoryPool::updateMemoryEfficiency() {
    std::lock_guard<std::mutex> metricsLock(metricsMutex_);
    
    // 计算内存效率：已使用内存 / (已使用内存 + 碎片内存)
    u64 usedAndFragmented = stats_.allocatedSize + stats_.fragmentedSize;
    if (usedAndFragmented > 0) {
        metrics_.memoryEfficiency = static_cast<f32>(stats_.allocatedSize) / usedAndFragmented;
    } else {
        metrics_.memoryEfficiency = 1.0f;
    }
}

u64 RHIAdaptiveMemoryPool::getCurrentTimestamp() const {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

u32 RHIAdaptiveMemoryPool::findFreePreallocatedBlock(u64 size, u64 alignment) {
    for (u32 blockHandle : preallocatedBlocks_) {
        MemoryBlock block = GetMemoryBlock(blockHandle);
        if (block.size >= size && 
            (alignment == 0 || (block.offset & (alignment - 1)) == 0)) {
            return blockHandle;
        }
    }
    return u32_invalid_id;
}

void RHIAdaptiveMemoryPool::addToPreallocatedPool(u32 blockHandle) {
    if (std::find(preallocatedBlocks_.begin(), preallocatedBlocks_.end(), blockHandle) == preallocatedBlocks_.end()) {
        preallocatedBlocks_.push_back(blockHandle);
    }
}

void RHIAdaptiveMemoryPool::removeFromPreallocatedPool(u32 blockHandle) {
    auto it = std::find(preallocatedBlocks_.begin(), preallocatedBlocks_.end(), blockHandle);
    if (it != preallocatedBlocks_.end()) {
        preallocatedBlocks_.erase(it);
    }
}

bool RHIAdaptiveMemoryPool::validateAllocationRecords() const {
    // 验证分配记录的一致性
    for (const auto& pair : allocationRecords_) {
        u32 blockHandle = pair.first;
        const AllocationRecord& record = pair.second;
        
        // 检查内存块是否有效
        if (!IsValidBlock(blockHandle)) {
            return false;
        }
        
        // 检查时间戳合理性
        if (record.timestamp == DEFAULT_TIMESTAMP) {
            return false;
        }
        
        // 检查大小合理性
        if (record.size == 0) {
            return false;
        }
    }
    
    return true;
}

bool RHIAdaptiveMemoryPool::validateHotspotConsistency() const {
    // 验证热点列表的一致性
    for (u32 blockHandle : hotspotBlocks_) {
        auto it = allocationRecords_.find(blockHandle);
        if (it == allocationRecords_.end() || 
            it->second.hotspotLevel < MemoryHotspotLevel::Hot) {
            return false;
        }
    }
    
    return true;
}

u32 RHIAdaptiveMemoryPool::allocateFromMemoryPool(u64 size, u64 alignment, GPUMemoryUsage usage) {
    // 简单的首次适应算法
    u64 alignedSize = RHIMemoryPool::AlignSize(size, alignment ? alignment : 256);
    
    for (size_t i = 0; i < freeBlocks_.size(); ++i) {
        u32 blockIndex = freeBlocks_[i];
        if (blockIndex >= memoryBlocks_.size()) continue;
        
        // 注意：不要在这里获取引用，因为 push_back 可能导致 vector 扩容使引用失效
        // MemoryBlock& block = memoryBlocks_[blockIndex];
        
        // 检查块是否空闲且大小足够
        if (memoryBlocks_[blockIndex].state == MemoryBlockState::Free) {
            u64 currentOffset = memoryBlocks_[blockIndex].offset;
            u64 currentSize = memoryBlocks_[blockIndex].size;
            u64 currentAlignment = memoryBlocks_[blockIndex].alignment;
            
            // 计算对齐后的起始位置
            u64 alignedOffset = RHIMemoryPool::AlignSize(currentOffset, alignment ? alignment : 256);
            u64 alignmentGap = alignedOffset - currentOffset;
            
            // 检查包含 Gap 后是否仍然足够
            if (currentSize >= alignedSize + alignmentGap) {
                // 1. 处理 Gap (如果有)
                if (alignmentGap > 0) {
                    MemoryBlock gapBlock;
                    gapBlock.offset = currentOffset;
                    gapBlock.size = alignmentGap;
                    gapBlock.alignment = currentAlignment;
                    gapBlock.state = MemoryBlockState::Free;
                    gapBlock.usage = GPUMemoryUsage::Unknown;
                    
                    // 添加到 memoryBlocks_ 末尾
                    memoryBlocks_.push_back(gapBlock);
                    // 添加到 freeBlocks_
                    freeBlocks_.push_back(static_cast<u32>(memoryBlocks_.size() - 1));
                }
                
                // 2. 处理剩余部分 (如果有)
                u64 remainingSize = currentSize - alignmentGap - alignedSize;
                if (remainingSize > 0) {
                    MemoryBlock remainderBlock;
                    remainderBlock.offset = alignedOffset + alignedSize;
                    remainderBlock.size = remainingSize;
                    remainderBlock.alignment = currentAlignment;
                    remainderBlock.state = MemoryBlockState::Free;
                    remainderBlock.usage = GPUMemoryUsage::Unknown;
                    
                    memoryBlocks_.push_back(remainderBlock);
                    freeBlocks_.push_back(static_cast<u32>(memoryBlocks_.size() - 1));
                }
                
                // 3. 更新当前块为 Allocated
                // 此时再次通过索引访问是安全的
                memoryBlocks_[blockIndex].offset = alignedOffset;
                memoryBlocks_[blockIndex].size = alignedSize;
                memoryBlocks_[blockIndex].state = MemoryBlockState::Allocated;
                memoryBlocks_[blockIndex].usage = usage;
                
                // 从 freeBlocks_ 中移除当前块
                freeBlocks_.erase(freeBlocks_.begin() + i);
                
                // 返回 Handle
                u32 handle = nextBlockHandle_++;
                handleToBlockIndex_[handle] = blockIndex;
                return handle;
            }
        }
    }
    
    return u32_invalid_id; // 分配失败
}

void RHIAdaptiveMemoryPool::deallocateFromMemoryPool(u32 blockHandle) {
    auto it = handleToBlockIndex_.find(blockHandle);
    if (it == handleToBlockIndex_.end()) {
        return; // 无效句柄
    }
    
    u32 blockIndex = it->second;
    if (blockIndex >= memoryBlocks_.size()) {
        // 异常情况，清理映射
        handleToBlockIndex_.erase(it);
        return;
    }
    
    MemoryBlock& block = memoryBlocks_[blockIndex];
    block.state = MemoryBlockState::Free;
    block.usage = GPUMemoryUsage::Unknown;
    
    // 添加到空闲列表
    freeBlocks_.push_back(blockIndex);
    
    // 移除映射
    handleToBlockIndex_.erase(it);
}

void RHIAdaptiveMemoryPool::logAdaptiveEvent(const char* event, const char* details) const {
    if (details) {
    } else {
    }
}

} // namespace primal::graphics::rhi