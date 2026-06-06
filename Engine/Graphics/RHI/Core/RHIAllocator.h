/**
 * @file RHIAllocator.h
 * @brief RHI对象分配器
 * @details 基于FreeList的线程安全对象分配器，支持性能统计和内存管理
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 1.4.0
 */

#pragma once

#include "CommonHeaders.h"
#include "Utilities/FreeList.h"
#include "RHIResource.h"
#include "RHICommand.h"
#include <shared_mutex>
#include <mutex>
#include <type_traits>
#include <iostream>

namespace primal::graphics::rhi {

/**
 * @brief 分配器统计信息
 */
struct AllocatorStats {
    std::atomic<u64> totalAllocated{0};        ///< 总分配次数
    std::atomic<u64> totalFreed{0};            ///< 总释放次数
    std::atomic<u64> activeAllocations{0};     ///< 当前活跃分配数
    std::atomic<u64> totalBytesAllocated{0};   ///< 当前分配的字节数（估算）
    
    // 拷贝构造函数需要显式定义以处理 atomic
    AllocatorStats() = default;
    AllocatorStats(const AllocatorStats& other) {
        totalAllocated = other.totalAllocated.load();
        totalFreed = other.totalFreed.load();
        activeAllocations = other.activeAllocations.load();
        totalBytesAllocated = other.totalBytesAllocated.load();
    }
    AllocatorStats& operator=(const AllocatorStats& other) {
        totalAllocated = other.totalAllocated.load();
        totalFreed = other.totalFreed.load();
        activeAllocations = other.activeAllocations.load();
        totalBytesAllocated = other.totalBytesAllocated.load();
        return *this;
    }
};

/**
 * @brief RHI对象分配器模板类
 * @tparam T 被管理的对象类型
 * @details 封装utl::free_list，提供线程安全访问、统计和管理功能
 */
template<typename T>
class RHIAllocator {
public:
    /**
     * @brief 构造函数
     */
    RHIAllocator() = default;

    /**
     * @brief 析构函数
     */
    ~RHIAllocator() {
        Destroy();
    }

    /**
     * @brief 初始化分配器
     * @return 初始化是否成功
     */
    bool Initialize() {
        // 当前实现不需要特殊初始化，预留接口
        return true;
    }

    /**
     * @brief 预分配容量
     * @param count 容量大小
     */
    void Reserve(u32 count) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _pool.reserve(count);
    }

    /**
     * @brief 销毁分配器并检查泄漏
     */
    void Destroy() {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        if (_stats.activeAllocations > 0) {
            // 在调试模式下输出警告
            // 由于这里没有日志系统，且不能使用cout（根据规范），我们仅依赖断言
            // 或者留给上层处理
            assert(_stats.activeAllocations == 0 && "Memory leak detected in RHIAllocator");
        }
        // free_list 析构会自动清理内存
    }

    /**
     * @brief 关闭分配器并释放所有活跃资源
     * @details 主动释放所有未释放的资源，防止内存泄漏。通常在设备关闭时调用。
     */
    void Shutdown() {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        u32 cap = _pool.capacity();
        // std::cout << "[RHIAllocator] Shutdown: capacity=" << cap << ", size=" << _pool.size() << std::endl;
        for (u32 i = 0; i < cap; ++i) {
            if (_pool.is_valid(i)) {
                FreeInternal(i);
            }
        }
        // std::cout << "[RHIAllocator] Shutdown complete. Final size=" << _pool.size() << std::endl;
    }

    /**
     * @brief 分配对象
     * @tparam Args 构造参数类型
     * @param args 构造参数
     * @return 对象的Handle (ID)
     */
    template<typename... Args>
    u32 Allocate(Args&&... args) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        u32 id = _pool.add(std::forward<Args>(args)...);
        
        if (sizeof(T) == 232) { // Trace MetalCommandBuffer
             // printf("Allocator Alloc: id=%u, T size=%zu\n", id, sizeof(T));
        }

        // 如果是 RHIResource 的子类，自动设置 Handle
        if constexpr (std::is_base_of_v<RHIResource, T>) {
            _pool[id].SetHandle(ResourceHandle(id));
        } else if constexpr (std::is_base_of_v<RHICommandBuffer, T>) {
            _pool[id].SetHandle(CommandBufferHandle(id));
        }
        
        _stats.totalAllocated++;
        _stats.activeAllocations++;
        _stats.totalBytesAllocated += sizeof(T);
        return id;
    }

    /**
     * @brief 释放对象
     * @param id 对象的Handle
     */
    void Free(u32 id) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        FreeInternal(id);
    }

    /**
     * @brief 获取对象指针
     * @param id 对象的Handle
     * @return 对象指针，如果ID无效可能触发断言（取决于free_list实现）
     */
    T* Get(u32 id) {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (id >= _pool.capacity()) return nullptr;
        if (!_pool.is_valid(id)) return nullptr;
        // free_list operator[] 包含断言检查
        return &_pool[id];
    }
    
    /**
     * @brief 获取当前统计信息
     * @return 统计结构副本
     */
    AllocatorStats GetStats() const {
        // 由于 AllocatorStats 内部是 atomic，直接拷贝是安全的（通过自定义拷贝构造）
        // 但为了保证一致性快照，我们不需要锁整个 _mutex，因为 atomic 保证了单个字段的原子性
        // 但不保证字段间的一致性。为了精确快照，还是加锁比较好。
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _stats;
    }

    /**
     * @brief 获取当前容量
     * @return 容量大小
     */
    u32 Capacity() const {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return _pool.capacity();
    }

    /**
     * @brief Iterate over all active (valid) objects
     * @param callback Called for each active object
     */
    template<typename F>
    void ForEach(F&& callback) {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        u32 cap = _pool.capacity();
        for (u32 i = 0; i < cap; ++i) {
            if (_pool.is_valid(i)) {
                callback(_pool[i]);
            }
        }
    }

    /**
     * @brief 执行内存碎片整理
     * @details 尝试缩减未使用的内存。注意：由于依赖底层实现，可能不会物理移动对象。
     */
    void Defragment() {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        // 目前 utl::free_list 不支持 shrink_to_fit。
        // 如果将来 utl::free_list 支持，可以在此调用。
        // 这是一个预留接口。
    }

private:
    /**
     * @brief 内部释放实现（无锁）
     */
    void FreeInternal(u32 id) {
        // 简单的范围检查
        if (id >= _pool.capacity()) return;
        
        if (sizeof(T) == 232) { // Trace MetalCommandBuffer
             // printf("Allocator Free: id=%u, T size=%zu\n", id, sizeof(T));
        }

        _pool.remove(id);
        _stats.totalFreed++;
        _stats.activeAllocations--;
        _stats.totalBytesAllocated -= sizeof(T);
    }

    primal::utl::free_list<T> _pool;
    mutable std::shared_mutex _mutex;
    AllocatorStats _stats;
};

} // namespace primal::graphics::rhi
