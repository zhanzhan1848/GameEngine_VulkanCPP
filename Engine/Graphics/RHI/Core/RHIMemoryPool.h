/**
 * @file RHIMemoryPool.h
 * @brief RHI内存池管理系统
 * @details 提供高效的GPU内存分配和管理功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "RHITypes.h"
#include "../../../Utilities/Vector.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <memory>

namespace primal::graphics::rhi {

// === 前向声明 ===

class RHIDevice;

/**
 * @brief 内存分配策略枚举
 * @details 定义不同的内存分配和管理策略
 */
enum class MemoryAllocationStrategy : uint8_t {
    Linear = 0,         ///< 线性分配，简单快速
    Buddy = 1,          ///< 伙伴系统，适合多种大小分配
    TLSF = 2,           ///< Two-Level Segregated Fit，高性能
    FreeList = 3,       ///< 自由链表，适合固定大小分配
    Pool = 4            ///< 内存池，预分配固定大小块
};

/**
 * @brief 内存块状态枚举
 */
enum class MemoryBlockState : uint8_t {
    Free = 0,           ///< 空闲
    Allocated = 1,      ///< 已分配
    Fragmented = 2,     ///< 碎片化
    Reserved = 3        ///< 预留
};

/**
 * @brief 内存块描述符
 * @details 描述内存块的基本信息
 */
struct MemoryBlock {
    uint64_t offset;            ///< 块在内存池中的偏移量
    uint64_t size;              ///< 块大小
    uint64_t alignment;         ///< 对齐要求
    MemoryBlockState state;     ///< 块状态
    GPUMemoryUsage usage;       ///< 内存用途类型
    uint32_t padding;           ///< 填充字节
    void* userData;             ///< 用户数据指针
    
    MemoryBlock() : offset(0), size(0), alignment(0), state(MemoryBlockState::Free),
                   usage(GPUMemoryUsage::Unknown), padding(0), userData(nullptr) {}
    
    MemoryBlock(uint64_t off, uint64_t sz, uint64_t align = 0)
        : offset(off), size(sz), alignment(align), state(MemoryBlockState::Free),
          usage(GPUMemoryUsage::Unknown), padding(0), userData(nullptr) {}
};

/**
 * @brief 内存池描述符
 * @details 定义内存池的配置参数
 */
struct MemoryPoolDesc {
    uint64_t poolSize;                  ///< 内存池总大小
    uint64_t blockSize;                 ///< 默认块大小
    uint64_t alignment;                 ///< 默认对齐要求
    MemoryAllocationStrategy strategy;  ///< 分配策略
    GPUMemoryUsage usage;                ///< 内存用途
    bool allowGrowth;                   ///< 是否允许动态增长
    bool threadSafe;                     ///< 是否线程安全
    uint32_t maxBlocks;                  ///< 最大块数量
    const char* name;                    ///< 内存池名称
    
    MemoryPoolDesc() : poolSize(64 * 1024 * 1024), blockSize(1024), alignment(256),
                      strategy(MemoryAllocationStrategy::FreeList), usage(GPUMemoryUsage::Default),
                      allowGrowth(false), threadSafe(true), maxBlocks(1024), name("RHIMemoryPool") {}
};

/**
 * @brief 内存统计信息
 * @details 用于内存使用情况分析和优化
 */
struct MemoryStats {
    uint64_t totalSize;                  ///< 总内存大小
    uint64_t allocatedSize;               ///< 已分配内存大小
    uint64_t freeSize;                    ///< 空闲内存大小
    uint64_t fragmentedSize;              ///< 碎片化内存大小
    uint32_t totalBlocks;                 ///< 总块数量
    uint32_t allocatedBlocks;             ///< 已分配块数量
    uint32_t freeBlocks;                  ///< 空闲块数量
    uint32_t fragmentedBlocks;            ///< 碎片化块数量
    float fragmentationRatio;            ///< 碎片化比例
    uint32_t allocationCount;             ///< 分配次数
    uint32_t deallocationCount;           ///< 释放次数
    uint64_t peakUsage;                   ///< 峰值使用量
    
    MemoryStats() : totalSize(0), allocatedSize(0), freeSize(0), fragmentedSize(0),
                    totalBlocks(0), allocatedBlocks(0), freeBlocks(0), fragmentedBlocks(0),
                    fragmentationRatio(0.0f), allocationCount(0), deallocationCount(0),
                    peakUsage(0) {}
};

/**
 * @brief RHI内存池基类
 * @details 提供GPU内存分配和管理的基础接口
 */
class RHIMemoryPool {
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param desc 内存池描述符
     */
    explicit RHIMemoryPool(RHIDevice& device, const MemoryPoolDesc& desc)
        : device_(device), desc_(desc), stats_(), initialized_(false) {}
    
    /**
     * @brief 虚析构函数
     */
    virtual ~RHIMemoryPool() {
        if (initialized_) {
            Destroy();
        }
    }
    
    // === 禁用拷贝，支持移动 ===
    
    RHIMemoryPool(const RHIMemoryPool&) = delete;
    RHIMemoryPool& operator=(const RHIMemoryPool&) = delete;
    
    RHIMemoryPool(RHIMemoryPool&& other) noexcept
        : device_(other.device_), desc_(std::move(other.desc_)), stats_(other.stats_),
          initialized_(other.initialized_) {
        other.initialized_ = false;
        other.stats_ = MemoryStats();
    }
    
    RHIMemoryPool& operator=(RHIMemoryPool&& other) noexcept {
        if (this != &other) {
            if (initialized_) {
                Destroy();
            }
            
            device_ = other.device_;
            desc_ = std::move(other.desc_);
            stats_ = other.stats_;
            initialized_ = other.initialized_;
            
            other.initialized_ = false;
            other.stats_ = MemoryStats();
        }
        return *this;
    }
    
    // === 核心接口方法 ===
    
    /**
     * @brief 初始化内存池
     * @return 初始化是否成功
     */
    virtual bool Initialize() = 0;
    
    /**
     * @brief 销毁内存池
     */
    virtual void Destroy() {
        if (initialized_) {
            destroyImpl();
            initialized_ = false;
        }
    }
    
    /**
     * @brief 分配内存块
     * @param size 需要分配的大小
     * @param alignment 对齐要求
     * @param usage 内存用途
     * @return 内存块句柄，失败返回0
     */
    virtual uint32_t Allocate(uint64_t size, uint64_t alignment = 0, GPUMemoryUsage usage = GPUMemoryUsage::Default) = 0;
    
    /**
     * @brief 释放内存块
     * @param blockHandle 内存块句柄
     * @return 释放是否成功
     */
    virtual bool Deallocate(uint32_t blockHandle) = 0;
    
    /**
     * @brief 重新分配内存块
     * @param blockHandle 内存块句柄
     * @param newSize 新的大小
     * @param newAlignment 新的对齐要求
     * @return 新的内存块句柄，失败返回0
     */
    virtual uint32_t Reallocate(uint32_t blockHandle, uint64_t newSize, uint64_t newAlignment = 0) = 0;
    
    /**
     * @brief 获取内存块信息
     * @param blockHandle 内存块句柄
     * @return 内存块描述符，失败返回空对象
     */
    virtual MemoryBlock GetMemoryBlock(uint32_t blockHandle) const = 0;
    
    /**
     * @brief 内存池整理（碎片回收）
     * @return 整理是否成功
     */
    virtual bool Defragment() = 0;
    
    /**
     * @brief 清空内存池
     * @return 清空是否成功
     */
    virtual bool Clear() = 0;
    
    // === 访问器方法 ===
    
    /**
     * @brief 检查是否已初始化
     * @return 初始化状态
     */
    bool IsInitialized() const { return initialized_; }
    
    /**
     * @brief 获取内存池描述符
     * @return 描述符的常量引用
     */
    const MemoryPoolDesc& GetDesc() const { return desc_; }
    
    /**
     * @brief 获取内存统计信息
     * @return 统计信息的常量引用
     */
    const MemoryStats& GetStats() const { return stats_; }
    
    /**
     * @brief 获取内存池大小
     * @return 内存池大小
     */
    uint64_t GetSize() const { return desc_.poolSize; }
    
    /**
     * @brief 获取可用内存大小
     * @return 可用内存大小
     */
    uint64_t GetAvailableSize() const { return stats_.freeSize; }
    
    /**
     * @brief 获取已使用内存大小
     * @return 已使用内存大小
     */
    uint64_t GetUsedSize() const { return stats_.allocatedSize; }
    
    /**
     * @brief 获取内存使用率
     * @return 使用率（0.0 - 1.0）
     */
    float GetUsageRatio() const {
        return desc_.poolSize > 0 ? static_cast<float>(stats_.allocatedSize) / static_cast<float>(desc_.poolSize) : 0.0f;
    }
    
    /**
     * @brief 获取碎片化比例
     * @return 碎片化比例（0.0 - 1.0）
     */
    float GetFragmentationRatio() const {
        return stats_.fragmentationRatio;
    }
    
    /**
     * @brief 检查是否有足够的内存
     * @param size 需要的大小
     * @param alignment 对齐要求
     * @return 是否有足够内存
     */
    bool HasEnoughMemory(uint64_t size, uint64_t alignment = 0) const {
        if (alignment > 0) {
            size = AlignSize(size, alignment);
        }
        return stats_.freeSize >= size;
    }
    
    /**
     * @brief 检查内存块是否有效
     * @param blockHandle 内存块句柄
     * @return 内存块是否有效
     */
    virtual bool IsValidBlock(uint32_t blockHandle) const = 0;
    
    /**
     * @brief 获取内存块的对齐后大小
     * @param size 原始大小
     * @param alignment 对齐要求
     * @return 对齐后的大小
     */
    static uint64_t AlignSize(uint64_t size, uint64_t alignment) {
        if (alignment == 0) return size;
        return (size + alignment - 1) & ~(alignment - 1);
    }
    
    // === 调试和诊断方法 ===
    
    /**
     * @brief 打印内存统计信息
     */
    void PrintStats() const;
    
    /**
     * @brief 打印内存块信息
     * @param blockHandle 内存块句柄
     */
    void PrintMemoryBlock(uint32_t blockHandle) const;
    
    /**
     * @brief 打印所有内存块信息
     */
    void PrintAllMemoryBlocks() const;
    
    /**
     * @brief 验证内存池一致性
     * @return 验证是否通过
     */
    virtual bool Validate() const = 0;
    
    /**
     * @brief 生成内存使用报告
     * @return 报告字符串
     */
    virtual std::string GenerateReport() const;

protected:
    // === 受保护的虚函数 ===
    
    virtual void destroyImpl() = 0;
    virtual void updateStats() = 0;
    
    // === 受保护的成员变量 ===
    
    RHIDevice& device_;                 ///< 设备引用
    MemoryPoolDesc desc_;               ///< 内存池描述符
    MemoryStats stats_;                 ///< 统计信息
    bool initialized_;                   ///< 初始化状态
    
    // === 受保护的辅助方法 ===
    
    /**
     * @brief 更新统计信息
     */
    void UpdateFragmentationRatio() {
        if (stats_.totalSize > 0) {
            stats_.fragmentationRatio = static_cast<float>(stats_.fragmentedSize) / static_cast<float>(stats_.totalSize);
        } else {
            stats_.fragmentationRatio = 0.0f;
        }
    }
    
    /**
     * @brief 更新峰值使用量
     */
    void UpdatePeakUsage() {
        if (stats_.allocatedSize > stats_.peakUsage) {
            stats_.peakUsage = stats_.allocatedSize;
        }
    }
    
    /**
     * @brief 检查对齐要求是否有效
     * @param alignment 对齐要求
     * @return 是否有效
     */
    bool IsValidAlignment(uint64_t alignment) const {
        return alignment == 0 || (alignment & (alignment - 1)) == 0;  // 必须是2的幂
    }
    
    /**
     * @brief 线程安全锁（如果需要）
     */
    class ScopedLock {
    public:
        explicit ScopedLock(RHIMemoryPool* pool) : pool_(pool) {
            if (pool_ && pool_->desc_.threadSafe) {
                pool_->mutex_.lock();
            }
        }
        
        ~ScopedLock() {
            if (pool_ && pool_->desc_.threadSafe) {
                pool_->mutex_.unlock();
            }
        }
        
    private:
        RHIMemoryPool* pool_;
    };
    
    friend class ScopedLock;
    
private:
    mutable std::mutex mutex_;  ///< 线程安全互斥锁
};

} // namespace primal::graphics::rhi