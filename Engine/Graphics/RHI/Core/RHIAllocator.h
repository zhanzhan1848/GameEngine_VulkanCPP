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

// MSVC 没有 __builtin_return_address；_ReturnAddress() 语义等价（返回调用方地址）。
// 仅用于 stale-handle 诊断打印，非功能路径。
#if defined(_MSC_VER)
  #include <intrin.h>
  #define PRIMAL_RHI_RETURN_ADDRESS() _ReturnAddress()
#else
  #define PRIMAL_RHI_RETURN_ADDRESS() __builtin_return_address(0)
#endif
#include "Utilities/FreeList.h"
#include "RHIResource.h"
#include "RHICommand.h"
#include <shared_mutex>
#include <mutex>
#include <type_traits>
#include <vector>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <typeinfo>

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
        if (_stats.activeAllocations > 0) {
            std::cerr << "[RHIAllocator] LEAK: " << _stats.activeAllocations.load()
                      << " active allocations (T=" << typeid(T).name()
                      << ", size=" << sizeof(T) << ")" << std::endl;
        }
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
        for (u32 i = 0; i < _pool.capacity(); ++i) {
            if (_pool.is_valid(i)) {
                FreeInternal(i);
            }
        }
        _pending_frees.clear();   // shutdown releases everything immediately
        _pending_retires.clear(); // R12-3.4: 已随上面全量 FreeInternal 释放
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

        // R12-3.4: 槽位代际登记（首生命周期 generation=1；0 保留给未编码句柄，
        // 使裸 id 构造的旧式句柄永不匹配活槽位）。
        if (id >= _generations.size()) _generations.resize(id + 1, 0);
        if (_generations[id] == 0) _generations[id] = 1;

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
     * @details R1/T0.13: slot release is deferred by kDeferredFreeLag frames
     *          once frame ticking is armed (see TickDeferredRelease). In-flight
     *          frames may still hold ResourceHandle(id); handing the same id to
     *          the next Allocate() aliases the old handle to a brand-new object
     *          (C1-H2). The slot stays valid (Get returns the old object) until
     *          the lag expires — GPU-safe by the same bound as the frame fence.
     */
    void Free(u32 id) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        if (!_deferred_release_armed_) {
            FreeInternal(id);
            return;
        }
        // Duplicate Free of an already-pending id is dropped here; the actual
        // pool slot is only removed once by TickDeferredRelease.
        for (const auto& p : _pending_frees) {
            if (p.id == id) return;
        }
        _pending_frees.push_back({id, _deferred_frame_});
    }

    /**
     * @brief 推进延迟释放时钟并回收过期槽位
     * @param frame 单调递增的帧号（各设备 beginFrame 驱动）
     */
    void TickDeferredRelease(u64 frame) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _deferred_release_armed_ = true;
        _deferred_frame_ = frame;
        for (auto it = _pending_frees.begin(); it != _pending_frees.end();) {
            if (frame >= it->frame + kDeferredFreeLag) {
                if (_pool.is_valid(it->id)) {
                    FreeInternal(it->id);
                }
                it = _pending_frees.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ====================================================================
    // R12-3.4: 句柄代际（handle generation）+ 延迟回收（retire 队列）
    //
    // 背景（R11 taskC 复检结论）：DestroyTexture 走立即 Free 时，槽位 id 当场
    // 回收；随后（场景加载尾/帧 0-1 的 GI 重初始化等）新分配拿到同一 id，已
    // 捕获旧句柄值的既存结构把新对象解析成语义别名（20 条布局错配验证错误）。
    // r11 的「id 不回池（永久毒化槽）」变体又被 utl::vector 增长搬移否决
    // （毒化槽上的 std::string 拷贝构造崩溃）。本机制两条防线：
    //   1. 代际：句柄 = (generation << 32) | slot id。槽位每次被回收复用
    //      generation+1，过期句柄经 GetChecked 解析为 nullptr（+env 门控日志），
    //      绝不解析成槽位的下一个对象。generation 存于独立平行数组，不触碰
    //      free_list 的毒化/空闲链协议（避开 r11 v2 的搬移崩溃模式）。
    //   2. retire 队列：Retire(id) 不立即释放——槽位对象保持存活（与基线
    //      「从未释放」期间语义一致）直到 TickRetires 推进 kRetireFreeLag 帧
    //      后才 FreeInternal（析构 → GC 延迟销毁原生资源）。in-flight 帧
    //      （MAX_FRAMES_IN_FLIGHT=3 的 per-slot fence 界）与紧随其后的批量
    //      分配都拿不到这些 id。独立于 PRIMAL_DEFERRED_FREE 的 Free() 延迟
    //      （那条路径默认关，Metal 有已知崩溃不启用），不影响既有 Free 语义。
    // ====================================================================

    /**
     * @brief 编码代际句柄 (generation << 32 | slot id)
     * @return 槽位有效时返回编码句柄；无效/已释放槽位返回 0
     * @note generation 从 1 起。0 保留 = 未编码句柄，永不匹配活槽位——
     *       强制所有句柄生产者走本接口，杜绝裸 id 句柄。
     */
    u64 MakeHandle(u32 id) {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (id >= _generations.size() || !_pool.is_valid(id)) return 0;
        return (static_cast<u64>(_generations[id]) << 32) | static_cast<u64>(id);
    }

    /**
     * @brief 代际校验解析
     * @return 活对象指针；句柄过期（代际不符/槽位空/未编码 gen=0）返回 nullptr
     * @note 过期命中在 PRIMAL_RHI_HANDLE_DIAG=1 时打 stderr 一行（默认关）。
     */
    T* GetChecked(u64 handle) {
        const u32 id = static_cast<u32>(handle);
        const u32 gen = static_cast<u32>(handle >> 32);
        std::shared_lock<std::shared_mutex> lock(_mutex);
        if (id >= _pool.capacity() || !_pool.is_valid(id)) return nullptr;
        if (id >= _generations.size() || gen != _generations[id]) {
            if (StaleHandleDiagEnabled()) {
                std::fprintf(stderr,
                    "[RHI-HANDLE] stale %s handle id=%u gen=%u currentGen=%u ra=%p — resolving null\n",
                    typeid(T).name(), id, gen,
                    (id < _generations.size()) ? _generations[id] : 0u,
                    PRIMAL_RHI_RETURN_ADDRESS());
            }
            return nullptr;
        }
        return &_pool[id];
    }

    /**
     * @brief 句柄代际有效性查询（不解析对象）
     */
    bool IsValidHandle(u64 handle) const {
        const u32 id = static_cast<u32>(handle);
        const u32 gen = static_cast<u32>(handle >> 32);
        std::shared_lock<std::shared_mutex> lock(_mutex);
        return id < _pool.capacity() && id < _generations.size() &&
               _pool.is_valid(id) && gen == _generations[id];
    }

    /**
     * @brief retire 队列长度（诊断用）
     */
    void PendingRetireCount(u64& outCount) const {
        std::shared_lock<std::shared_mutex> lock(_mutex);
        outCount = _pending_retires.size();
    }

    /**
     * @brief R12-3.4: 延迟回收——把槽位排入 retire 队列
     * @details 槽位对象在 lag 到期前保持存活（Get 仍返回原对象，与基线
     *          「未释放」期间语义一致）；到期后由 TickRetires 执行
     *          FreeInternal（析构 + generation 推进）。重复 Retire 与
     *          Retire 后又显式 Free 均安全（tick 时 is_valid 复查）。
     */
    void Retire(u32 id) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        if (id >= _pool.capacity() || !_pool.is_valid(id)) return;
        for (const auto& p : _pending_retires) {
            if (p.id == id) return;
        }
        _pending_retires.push_back({id, _retire_frame_});
    }

    /**
     * @brief 推进 retire 时钟并回收到期槽位（不 arm Free() 延迟）
     * @param frame 单调递增的帧号（VulkanDevice::beginFrameImpl 每帧驱动）
     */
    void TickRetires(u64 frame) {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        _retire_frame_ = frame;
        u32 reclaimed = 0;
        for (auto it = _pending_retires.begin(); it != _pending_retires.end();) {
            if (frame >= it->frame + kRetireFreeLag) {
                if (_pool.is_valid(it->id)) {
                    FreeInternal(it->id);
                    ++reclaimed;
                }
                it = _pending_retires.erase(it);
            } else {
                ++it;
            }
        }
        if (reclaimed > 0 && StaleHandleDiagEnabled()) {
            std::fprintf(stderr, "[RHI-HANDLE] TickRetires frame=%llu reclaimed=%u pending=%zu\n",
                         static_cast<unsigned long long>(frame), reclaimed, _pending_retires.size());
        }
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

        _pool.remove(id);
        // R12-3.4: 槽位回收即推进代际——这一生命周期的所有句柄从现在起过期。
        if (id < _generations.size()) ++_generations[id];
        _stats.totalFreed++;
        _stats.activeAllocations--;
        _stats.totalBytesAllocated -= sizeof(T);
    }

    /// R12-3.4: 过期句柄诊断开关（默认关；见 GetChecked）
    static bool StaleHandleDiagEnabled() {
        static const bool on = std::getenv("PRIMAL_RHI_HANDLE_DIAG") != nullptr;
        return on;
    }

    // R1/T0.13: deferred slot release state
    static constexpr u64 kDeferredFreeLag = 3;   // frames a freed slot stays reserved
    struct PendingFree { u32 id; u64 frame; };
    bool _deferred_release_armed_ = false;       // armed on first TickDeferredRelease
    u64 _deferred_frame_ = 0;
    std::vector<PendingFree> _pending_frees;     // guarded by _mutex

    // R12-3.4: retire 队列（独立时钟，不 arm Free() 延迟）+ 槽位代际平行数组
    static constexpr u64 kRetireFreeLag = 3;     // frames a retired slot stays alive
    std::vector<PendingFree> _pending_retires;   // guarded by _mutex
    u64 _retire_frame_ = 0;                      // retire 时钟（TickRetires 推进）
    std::vector<u32> _generations;               // guarded by _mutex; slot id → generation

    primal::utl::free_list<T> _pool;
    mutable std::shared_mutex _mutex;
    AllocatorStats _stats;
};

} // namespace primal::graphics::rhi
