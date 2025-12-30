/**
 * @file RHIMemoryPool.cpp
 * @brief RHI内存池管理系统实现
 * @details 内存池基类的非虚函数实现和工具函数
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#include "RHIMemoryPool.h"
#include "RHIDevice.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace primal::graphics::rhi {

// === 内存池工厂实现 ===

/**
 * @brief 内存池工厂实现
 */
class MemoryPoolFactoryImpl {
public:
    /**
     * @brief 创建内存池
     * @param device 设备引用
     * @param desc 内存池描述符
     * @return 内存池指针，失败返回nullptr
     */
    static std::unique_ptr<RHIMemoryPool> CreateMemoryPool(RHIDevice& device, const MemoryPoolDesc& desc) {
        // 根据策略类型创建不同的内存池
        // 这里返回nullptr，需要在具体平台实现中创建对应的内存池
        (void)device;
        (void)desc;
        return nullptr;
    }
    
    /**
     * @brief 获取推荐的内存池配置
     * @param usage 内存用途
     * @param size 预期大小
     * @return 推荐的内存池描述符
     */
    static MemoryPoolDesc GetRecommendedPoolDesc(GPUMemoryUsage usage, uint64_t size) {
        MemoryPoolDesc desc;
        
        switch (usage) {
            case GPUMemoryUsage::Immutable:
                desc.strategy = MemoryAllocationStrategy::Linear;
                desc.allowGrowth = false;
                desc.alignment = 256;
                break;
                
            case GPUMemoryUsage::Dynamic:
                desc.strategy = MemoryAllocationStrategy::FreeList;
                desc.allowGrowth = true;
                desc.alignment = 256;
                break;
                
            case GPUMemoryUsage::Readback:
                desc.strategy = MemoryAllocationStrategy::Linear;
                desc.allowGrowth = false;
                desc.alignment = 256;
                break;
                
            case GPUMemoryUsage::Staging:
                desc.strategy = MemoryAllocationStrategy::Pool;
                desc.allowGrowth = true;
                desc.alignment = 256;
                desc.blockSize = 64 * 1024;  // 64KB blocks
                break;
                
            default:
                desc.strategy = MemoryAllocationStrategy::FreeList;
                desc.allowGrowth = true;
                desc.alignment = 256;
                break;
        }
        
        // 根据大小调整池大小
        if (size > 0) {
            desc.poolSize = std::max(size, static_cast<uint64_t>(64 * 1024 * 1024));  // 最小64MB
        }
        
        return desc;
    }
};

// === 内存池工厂静态方法 ===

std::unique_ptr<RHIMemoryPool> MemoryPoolFactory::CreateMemoryPool(RHIDevice& device, const MemoryPoolDesc& desc) {
    return MemoryPoolFactoryImpl::CreateMemoryPool(device, desc);
}

// === RHIMemoryPool基类方法实现 ===

void RHIMemoryPool::PrintStats() const {
    const auto& stats = GetStats();
    const auto& desc = GetDesc();
    
    printf("=== Memory Pool Statistics ===\n");
    printf("Name: %s\n", desc.name);
    printf("Strategy: %d\n", static_cast<int>(desc.strategy));
    printf("Total Size: %llu bytes (%.2f MB)\n", 
           static_cast<unsigned long long>(stats.totalSize),
           stats.totalSize / (1024.0f * 1024.0f));
    printf("Allocated: %llu bytes (%.2f MB) - %.1f%%\n",
           static_cast<unsigned long long>(stats.allocatedSize),
           stats.allocatedSize / (1024.0f * 1024.0f),
           GetUsageRatio() * 100.0f);
    printf("Free: %llu bytes (%.2f MB) - %.1f%%\n",
           static_cast<unsigned long long>(stats.freeSize),
           stats.freeSize / (1024.0f * 1024.0f),
           (desc.poolSize > 0) ? (stats.freeSize * 100.0f / desc.poolSize) : 0.0f);
    printf("Fragmented: %llu bytes (%.2f MB) - %.1f%%\n",
           static_cast<unsigned long long>(stats.fragmentedSize),
           stats.fragmentedSize / (1024.0f * 1024.0f),
           stats.fragmentationRatio * 100.0f);
    printf("Blocks: %u total, %u allocated, %u free, %u fragmented\n",
           stats.totalBlocks, stats.allocatedBlocks, stats.freeBlocks, stats.fragmentedBlocks);
    printf("Operations: %u allocations, %u deallocations\n",
           stats.allocationCount, stats.deallocationCount);
    printf("Peak Usage: %llu bytes (%.2f MB)\n",
           static_cast<unsigned long long>(stats.peakUsage),
           stats.peakUsage / (1024.0f * 1024.0f));
    printf("================================\n");
}

void RHIMemoryPool::PrintMemoryBlock(uint32_t blockHandle) const {
    if (!IsValidBlock(blockHandle)) {
        printf("Invalid memory block handle: %u\n", blockHandle);
        return;
    }
    
    MemoryBlock block = GetMemoryBlock(blockHandle);
    
    printf("=== Memory Block %u ===\n", blockHandle);
    printf("Offset: %llu bytes\n", static_cast<unsigned long long>(block.offset));
    printf("Size: %llu bytes (%.2f KB)\n",
           static_cast<unsigned long long>(block.size),
           block.size / 1024.0f);
    printf("Alignment: %llu bytes\n", static_cast<unsigned long long>(block.alignment));
    printf("State: %d\n", static_cast<int>(block.state));
    printf("Usage: %d\n", static_cast<int>(block.usage));
    printf("User Data: %p\n", block.userData);
    printf("========================\n");
}

void RHIMemoryPool::PrintAllMemoryBlocks() const {
    printf("=== All Memory Blocks ===\n");
    
    // 这个方法需要在派生类中实现，因为基类不知道具体的块存储方式
    // 这里只是提供一个框架
    printf("This method should be implemented in derived classes.\n");
    
    printf("==========================\n");
}

std::string RHIMemoryPool::GenerateReport() const {
    std::ostringstream report;
    const auto& stats = GetStats();
    const auto& desc = GetDesc();
    
    report << "=== Memory Pool Report ===\n";
    report << "Name: " << desc.name << "\n";
    report << "Strategy: " << static_cast<int>(desc.strategy) << "\n";
    report << "Pool Size: " << desc.poolSize << " bytes (" 
           << std::fixed << std::setprecision(2) << (desc.poolSize / (1024.0f * 1024.0f)) << " MB)\n";
    report << "Block Size: " << desc.blockSize << " bytes\n";
    report << "Alignment: " << desc.alignment << " bytes\n";
    report << "Thread Safe: " << (desc.threadSafe ? "Yes" : "No") << "\n";
    report << "Allow Growth: " << (desc.allowGrowth ? "Yes" : "No") << "\n";
    report << "Max Blocks: " << desc.maxBlocks << "\n";
    report << "\n";
    
    report << "=== Usage Statistics ===\n";
    report << "Allocated: " << stats.allocatedSize << " bytes (" 
           << std::fixed << std::setprecision(2) << (stats.allocatedSize / (1024.0f * 1024.0f)) 
           << " MB, " << (GetUsageRatio() * 100.0f) << "%)\n";
    report << "Free: " << stats.freeSize << " bytes (" 
           << std::fixed << std::setprecision(2) << (stats.freeSize / (1024.0f * 1024.0f)) << " MB)\n";
    report << "Fragmented: " << stats.fragmentedSize << " bytes (" 
           << std::fixed << std::setprecision(2) << (stats.fragmentedSize / (1024.0f * 1024.0f)) 
           << " MB, " << (stats.fragmentationRatio * 100.0f) << "%)\n";
    report << "\n";
    
    report << "=== Block Statistics ===\n";
    report << "Total Blocks: " << stats.totalBlocks << "\n";
    report << "Allocated Blocks: " << stats.allocatedBlocks << "\n";
    report << "Free Blocks: " << stats.freeBlocks << "\n";
    report << "Fragmented Blocks: " << stats.fragmentedBlocks << "\n";
    report << "\n";
    
    report << "=== Operation Statistics ===\n";
    report << "Allocation Count: " << stats.allocationCount << "\n";
    report << "Deallocation Count: " << stats.deallocationCount << "\n";
    report << "Peak Usage: " << stats.peakUsage << " bytes (" 
           << std::fixed << std::setprecision(2) << (stats.peakUsage / (1024.0f * 1024.0f)) << " MB)\n";
    report << "\n";
    
    // 计算一些额外的统计信息
    if (stats.allocationCount > 0) {
        float avgBlockSize = static_cast<float>(stats.allocatedSize) / static_cast<float>(stats.allocatedBlocks);
        report << "Average Block Size: " << std::fixed << std::setprecision(2) << avgBlockSize << " bytes\n";
    }
    
    float efficiency = (stats.totalSize > 0) ? 
        (static_cast<float>(stats.allocatedSize) / static_cast<float>(stats.totalSize)) * 100.0f : 0.0f;
    report << "Memory Efficiency: " << std::fixed << std::setprecision(2) << efficiency << "%\n";
    
    report << "==========================\n";
    
    return report.str();
}

// === 内存验证工具 ===

/**
 * @brief 验证内存块对齐
 * @param block 内存块
 * @return 对齐是否正确
 */
bool ValidateMemoryBlockAlignment(const MemoryBlock& block) {
    if (block.alignment == 0) return true;
    return (block.offset & (block.alignment - 1)) == 0;
}

/**
 * @brief 验证内存块大小
 * @param block 内存块
 * @param poolSize 内存池大小
 * @return 大小是否有效
 */
bool ValidateMemoryBlockSize(const MemoryBlock& block, uint64_t poolSize) {
    return block.offset < poolSize && 
           (block.offset + block.size) <= poolSize &&
           block.size > 0;
}

/**
 * @brief 检查内存块是否重叠
 * @param block1 第一个内存块
 * @param block2 第二个内存块
 * @return 是否重叠
 */
bool CheckMemoryBlockOverlap(const MemoryBlock& block1, const MemoryBlock& block2) {
    if (block1.state != MemoryBlockState::Allocated || 
        block2.state != MemoryBlockState::Allocated) {
        return false;
    }
    
    uint64_t end1 = block1.offset + block1.size;
    uint64_t end2 = block2.offset + block2.size;
    
    return !(block1.offset >= end2 || block2.offset >= end1);
}

// === 内存统计工具 ===

/**
 * @brief 计算内存使用效率
 * @param stats 内存统计信息
 * @return 使用效率百分比（0-100）
 */
float CalculateMemoryEfficiency(const MemoryStats& stats) {
    if (stats.totalSize == 0) return 0.0f;
    return (static_cast<float>(stats.allocatedSize) / static_cast<float>(stats.totalSize)) * 100.0f;
}

/**
 * @brief 计算碎片化程度
 * @param stats 内存统计信息
 * @return 碎片化程度（0-1，0表示无碎片，1表示完全碎片化）
 */
float CalculateFragmentationLevel(const MemoryStats& stats) {
    if (stats.freeSize == 0) return 0.0f;
    return stats.fragmentationRatio;
}

/**
 * @brief 获取内存使用等级
 * @param usageRatio 使用比例（0-1）
 * @return 使用等级描述
 */
const char* GetMemoryUsageLevel(float usageRatio) {
    if (usageRatio < 0.5f) return "Low";
    if (usageRatio < 0.8f) return "Medium";
    if (usageRatio < 0.9f) return "High";
    return "Critical";
}

/**
 * @brief 获取碎片化等级
 * @param fragmentationRatio 碎片化比例（0-1）
 * @return 碎片化等级描述
 */
const char* GetFragmentationLevel(float fragmentationRatio) {
    if (fragmentationRatio < 0.1f) return "Low";
    if (fragmentationRatio < 0.3f) return "Medium";
    if (fragmentationRatio < 0.5f) return "High";
    return "Critical";
}

// === 内存池建议系统 ===

/**
 * @brief 生成内存池优化建议
 * @param stats 内存统计信息
 * @return 优化建议列表
 */
utl::vector<std::string> GenerateOptimizationSuggestions(const MemoryStats& stats) {
    utl::vector<std::string> suggestions;
    
    // 检查内存使用率
    float usageRatio = (stats.totalSize > 0) ? 
        static_cast<float>(stats.allocatedSize) / static_cast<float>(stats.totalSize) : 0.0f;
    
    if (usageRatio > 0.9f) {
        suggestions.push_back("Memory usage is critical. Consider increasing pool size or implementing growth.");
    } else if (usageRatio < 0.3f) {
        suggestions.push_back("Memory usage is low. Consider reducing pool size to save memory.");
    }
    
    // 检查碎片化
    if (stats.fragmentationRatio > 0.3f) {
        suggestions.push_back("High fragmentation detected. Consider defragmentation or using a different allocation strategy.");
    }
    
    // 检查块数量
    if (stats.freeBlocks > stats.allocatedBlocks * 2) {
        suggestions.push_back("Too many free blocks. Consider compaction or reducing initial block count.");
    }
    
    // 检查分配/释放模式
    if (stats.allocationCount > stats.deallocationCount * 10) {
        suggestions.push_back("High allocation rate with low deallocation. Check for memory leaks.");
    }
    
    return suggestions;
}

/**
 * @brief 获取推荐的分配策略
 * @param usage 内存用途
 * @param expectedUsage 预期使用模式
 * @return 推荐的分配策略
 */
MemoryAllocationStrategy GetRecommendedAllocationStrategy(GPUMemoryUsage usage, 
                                                          const char* expectedUsage) {
    // 根据内存用途和使用模式推荐策略
    switch (usage) {
        case GPUMemoryUsage::Immutable:
            return MemoryAllocationStrategy::Linear;
            
        case GPUMemoryUsage::Dynamic:
            if (expectedUsage && strstr(expectedUsage, "frequent")) {
                return MemoryAllocationStrategy::Pool;
            }
            return MemoryAllocationStrategy::FreeList;
            
        case GPUMemoryUsage::Readback:
            return MemoryAllocationStrategy::Linear;
            
        case GPUMemoryUsage::Staging:
            return MemoryAllocationStrategy::Pool;
            
        default:
            return MemoryAllocationStrategy::FreeList;
    }
}

} // namespace primal::graphics::rhi