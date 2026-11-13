/**
 * @file VulkanHangDiag.h
 * @brief Windows Vulkan 卡死排查的 fail-fast 诊断（R10 taskC，2026-11-13）
 *
 * 全部功能由 PRIMAL_VK_HANG_DIAG=1 门控，默认关闭。门控关闭时：
 *   - Enabled() 是唯一的额外开销（进程内一次性 getenv 缓存）；
 *   - 所有包装器（ScopedHangWatch / BoundedWaitFence / ReportDeviceLost /
 *     Note* 追踪）立即返回，不触碰任何 Vulkan 对象、不创建线程、不打日志，
 *     代码路径行为与引入本文件之前逐字节一致（Mac 生产路径零影响）。
 *
 * R12-3.2 增补：PRIMAL_VK_MEM_BUDGET_DIAG=1（同样默认关闭）为第二条独立
 * 门——仅启用 NoteInit/NoteFrameBegin 的 memory-budget 周期打印
 * （[VK-MEMBUDGET] 前缀，每 30 帧），不含看门狗/退出。两门同开时 hang-diag
 * 行为不变（budget 周期行走 [VK-HANG] 原格式，不重复打印）。
 *
 * 开启时（仅 Windows 诊断跑）：
 *   - 每个无界阻塞等待（acquire UINT64_MAX / 大超时 fence / queueWaitIdle /
 *     query WAIT_BIT / present / submit）入口打 [VK-HANG] wait kind=…，
 *     每 2s 打一条 still-waiting 进度行；
 *   - PRIMAL_VK_HANG_TIMEOUT_SECS（默认 20s，范围 2..3600）超时：dump
 *     诊断块（近端 fence 状态、acquire/present/submit 计数、
 *     VK_EXT_memory_budget 各 heap used/budget）+ [VK-HANG] timeout +
 *     std::_Exit(12)；
 *   - 任何关键 Vulkan 调用返回 VK_ERROR_DEVICE_LOST：
 *     [VK-DEVICE-LOST] where=… result=… + std::_Exit(13)（替代无限等待）；
 *   - device init 时查询并打印 memory budget 扩展支持与初始余量；
 *     每 30 帧打印一次预算（NoteFrameBegin）。
 *
 * watchdog（PRIMAL_SPONZA_WATCHDOG_SECS, exit 7）保留不动作为后盾——本
 * 诊断的目标是在 20s 内定位卡点而不是等 300s。
 */
#ifndef PRIMAL_GRAPHICS_RHI_VULKAN_HANG_DIAG_H
#define PRIMAL_GRAPHICS_RHI_VULKAN_HANG_DIAG_H

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>

namespace primal::graphics::rhi::hangdiag {

// ---------------------------------------------------------------------------
// 门控与参数
// ---------------------------------------------------------------------------
inline bool Enabled() {
    static const bool on = [] {
        const char* e = std::getenv("PRIMAL_VK_HANG_DIAG");
        return e && std::strcmp(e, "1") == 0;
    }();
    return on;
}

// R12-3.2（2026-11-13，Windows GI-off f301 AV）：独立于 hang-diag 的轻量
// 显存预算日志门，PRIMAL_VK_MEM_BUDGET_DIAG=1 开启，默认关闭。只做两件事：
// NoteInit 登记 phys/device 并打印初始预算、NoteFrameBegin 每 30 帧打印
// VK_EXT_memory_budget 各 heap used/budget（[VK-MEMBUDGET] 前缀）。无看门狗
// 线程、无 still-waiting 行、无 _Exit——给 r13 Windows 复跑在 AV/挂死点附近
// 留下"是否显存耗尽"的直接证据（r11/r12 的 gioff 症状——帧 2 死等与帧 301
// AV——都指向 6GB 卡显存超卖，但日志里没有一条内存证据）。扩展由
// VulkanDevice::createLogicalDevice 在本门开启时一并启用（budget 值需扩展
// 启用后才保证有效）。
inline bool BudgetDiagEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("PRIMAL_VK_MEM_BUDGET_DIAG");
        return e && std::strcmp(e, "1") == 0;
    }();
    return on;
}

inline uint32_t TimeoutSecs() {
    static const uint32_t secs = [] {
        const char* e = std::getenv("PRIMAL_VK_HANG_TIMEOUT_SECS");
        long v = e ? std::atol(e) : 20;
        if (v < 2) v = 2;
        if (v > 3600) v = 3600;
        return static_cast<uint32_t>(v);
    }();
    return secs;
}

// ---------------------------------------------------------------------------
// 追踪状态（仅 Enabled() 时写入；类型定义无条件存在以保持零开销路径简单）
// ---------------------------------------------------------------------------
inline std::atomic<uint64_t> g_frame{0};                  // BeginFrame 计数（device 层）
inline std::atomic<uint64_t> g_submits{0};                // 累计 vkQueueSubmit 成功次数
inline std::atomic<uint64_t> g_lastSubmitCmdCount{0};     // 最近一次提交的 cmd buffer 数
inline std::atomic<int64_t> g_lastAcquireFrame{-1};      // 最近一次 acquire 成功时的帧号
inline std::atomic<uint32_t> g_lastAcquireImage{0xFFFFFFFFu};
inline std::atomic<int64_t> g_lastPresentFrame{-1};      // 最近一次 present 成功时的帧号
inline std::atomic<uint32_t> g_lastPresentImage{0xFFFFFFFFu};

// 近端 signal fence 环形登记（dump 时逐个 vkGetFenceStatus）。
// 必须配对 RetireFence：所有 vkDestroyFence 点先 retire 再销毁，否则 dump
// 会查询已销毁句柄（validation 报错；2026-11-13 Mac 实测 dump 内卡死）。
constexpr uint32_t kFenceRingSize = 8;
struct FenceRingEntry {
    VkFence fence;
    std::atomic<bool> alive;
};
inline FenceRingEntry g_fenceRing[kFenceRingSize]{};
inline std::atomic<uint32_t> g_fenceRingNext{0};

// device/physical device 引用（NoteInit 登记，dump/预算打印用）
inline VkDevice g_dev{VK_NULL_HANDLE};
inline VkPhysicalDevice g_phys{VK_NULL_HANDLE};
inline std::atomic<int32_t> g_memoryBudgetSupported{-1};  // -1=未查询 0/1

inline void NoteSignalFence(VkFence f) {
    if (!Enabled() || f == VK_NULL_HANDLE) return;
    const uint32_t i = g_fenceRingNext.fetch_add(1) % kFenceRingSize;
    g_fenceRing[i].fence = f;
    g_fenceRing[i].alive.store(true);
}

inline void RetireFence(VkFence f) {
    if (!Enabled() || f == VK_NULL_HANDLE) return;
    for (uint32_t i = 0; i < kFenceRingSize; ++i) {
        if (g_fenceRing[i].fence == f && g_fenceRing[i].alive.load()) {
            g_fenceRing[i].alive.store(false);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// memory budget（VK_EXT_memory_budget）
// ---------------------------------------------------------------------------
inline bool MemoryBudgetSupported(VkPhysicalDevice phys) {
    if (phys == VK_NULL_HANDLE) return false;
    // vkEnumerateDeviceExtensionProperties 不要求扩展已启用，可直接查。
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    // 栈上 256 项足够（spec 上限 VK_MAX_EXTENSION_NAME 无关，常见 <200）。
    VkExtensionProperties ext[256];
    if (count > 256) count = 256;
    if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, ext) != VK_SUCCESS)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(ext[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0)
            return true;
    }
    return false;
}

inline void PrintMemoryBudget(const char* tag, const char* prefix = "[VK-HANG]") {
    if (g_phys == VK_NULL_HANDLE) return;
    if (g_memoryBudgetSupported.load() < 0) {
        g_memoryBudgetSupported.store(MemoryBudgetSupported(g_phys) ? 1 : 0);
    }
    fprintf(stderr, "%s %s membudget ext=%s\n", prefix, tag,
            g_memoryBudgetSupported.load() ? "VK_EXT_memory_budget" : "UNSUPPORTED");
    if (!g_memoryBudgetSupported.load()) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    props2.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(g_phys, &props2);
    for (uint32_t i = 0; i < props2.memoryProperties.memoryHeapCount; ++i) {
        const double usedMB = static_cast<double>(budget.heapUsage[i]) / (1024.0 * 1024.0);
        const double budMB = static_cast<double>(budget.heapBudget[i]) / (1024.0 * 1024.0);
        fprintf(stderr, "%s %s membudget heap=%u used=%.1fMB budget=%.1fMB%s frame=%llu\n",
                prefix, tag, i, usedMB, budMB,
                (props2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    ? " (device-local)" : "",
                static_cast<unsigned long long>(g_frame.load()));
    }
}

// ---------------------------------------------------------------------------
// 状态 dump + 退出
// ---------------------------------------------------------------------------
inline void DumpState(const char* where) {
    fprintf(stderr,
            "[VK-HANG] dump where=%s frame=%llu submits=%llu lastSubmitCmds=%llu "
            "lastAcquireFrame=%lld img=%u lastPresentFrame=%lld img=%u\n",
            where,
            static_cast<unsigned long long>(g_frame.load()),
            static_cast<unsigned long long>(g_submits.load()),
            static_cast<unsigned long long>(g_lastSubmitCmdCount.load()),
            static_cast<long long>(g_lastAcquireFrame.load()),
            g_lastAcquireImage.load(),
            static_cast<long long>(g_lastPresentFrame.load()),
            g_lastPresentImage.load());
    // 近端 fence 状态（vkGetFenceStatus 不阻塞；只查存活 fence——已销毁
    // 句柄在 validation 下会报错甚至卡死 dump，见 RetireFence 注释）
    if (g_dev != VK_NULL_HANDLE) {
        const uint32_t next = g_fenceRingNext.load();
        for (uint32_t k = 0; k < kFenceRingSize; ++k) {
            const uint32_t i = (next + kFenceRingSize - 1 - k) % kFenceRingSize;  // 新→旧
            const VkFence f = g_fenceRing[i].fence;
            if (f == VK_NULL_HANDLE || !g_fenceRing[i].alive.load()) continue;
            const VkResult st = vkGetFenceStatus(g_dev, f);
            fprintf(stderr, "[VK-HANG] fence[%u]=0x%llx status=%s (%d)\n", i,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(f)),
                    st == VK_SUCCESS ? "SIGNALED"
                    : st == VK_NOT_READY ? "UNSIGNALED"
                    : st == VK_ERROR_DEVICE_LOST ? "DEVICE_LOST" : "OTHER",
                    static_cast<int>(st));
        }
    }
    PrintMemoryBudget(where);
}

inline void ExitTimeout(const char* where) {
    DumpState(where);
    fprintf(stderr, "[VK-HANG] timeout where=%s after %us (PRIMAL_VK_HANG_TIMEOUT_SECS) — _Exit(12)\n",
            where, TimeoutSecs());
    std::fflush(stderr);
    std::_Exit(12);
}

// ---------------------------------------------------------------------------
// device lost 闸门
// ---------------------------------------------------------------------------
inline void ReportDeviceLost(const char* where, VkResult res) {
    if (!Enabled()) return;  // 门控关闭：完全无操作（行为零变化）
    fprintf(stderr, "[VK-DEVICE-LOST] where=%s result=%d — _Exit(13)\n", where, static_cast<int>(res));
    DumpState(where);
    std::fflush(stderr);
    std::_Exit(13);
}

// ---------------------------------------------------------------------------
// 有界 fence 等待（2s 步进；成功返回 true；其他非超时错误返回 false 交回
// 调用方既有错误路径；超时/lost 由本函数终止进程）
// ---------------------------------------------------------------------------
inline bool BoundedWaitFence(VkDevice dev, const char* where, VkFence fence) {
    // 注意：无入口打印——正常路径（fence 已/很快 signaled）必须零输出零噪声；
    // 真卡死时第一条 still-waiting（2s）即给出 where/kind 定位信息。
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const VkResult res = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL);
        if (res == VK_SUCCESS) return true;
        if (res == VK_ERROR_DEVICE_LOST) ReportDeviceLost(where, res);
        if (res != VK_TIMEOUT) return false;  // 其它错误：维持调用方原语义
        const auto el = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (el >= static_cast<long long>(TimeoutSecs())) ExitTimeout(where);
        fprintf(stderr, "[VK-HANG] still-waiting kind=fence where=%s elapsed=%llds frame=%llu fence=0x%llx\n",
                where, static_cast<long long>(el),
                static_cast<unsigned long long>(g_frame.load()),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fence)));
    }
}

// ---------------------------------------------------------------------------
// 不可分段调用（vkQueueWaitIdle / vkQueuePresentKHR / vkQueueSubmit）的
// 看门狗线程：入口打一条，每 2s 一条 still-waiting，超时 dump + _Exit(12)。
// 作用域结束正常返回则停止线程。
// ---------------------------------------------------------------------------
class ScopedHangWatch {
public:
    ScopedHangWatch(const char* kind, const char* detail)
        : kind_(kind), detail_(detail) {
        if (!Enabled()) return;
        active_ = true;
        // 无入口打印（高频调用零噪声）：真卡死时 2s 的 still-waiting 行由
        // 看门狗线程给出 kind/detail 定位。
        thread_ = std::thread([this] { WatchLoop(); });
    }
    ~ScopedHangWatch() {
        if (!active_) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_.store(true);
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ScopedHangWatch(const ScopedHangWatch&) = delete;
    ScopedHangWatch& operator=(const ScopedHangWatch&) = delete;

private:
    void WatchLoop() {
        // 可中断 2s 步进：调用快速完成时析构立即唤醒本线程（零额外延迟），
        // 真卡死时每 2s 打一条 still-waiting，超阈值 dump + _Exit(12)。
        const uint64_t limit = TimeoutSecs();
        std::unique_lock<std::mutex> lk(mu_);
        for (uint64_t s = 0; s < limit; s += 2) {
            if (cv_.wait_for(lk, std::chrono::seconds(2), [this] { return stop_.load(); }))
                return;
            fprintf(stderr, "[VK-HANG] still-waiting kind=%s detail=%s elapsed=%llus frame=%llu\n",
                    kind_, detail_, static_cast<unsigned long long>(s + 2),
                    static_cast<unsigned long long>(g_frame.load()));
        }
        if (stop_.load()) return;
        lk.unlock();
        ExitTimeout(kind_);
    }
    const char* kind_;
    const char* detail_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    bool active_{false};
};

// ---------------------------------------------------------------------------
// 生命周期追踪
// ---------------------------------------------------------------------------
inline void NoteInit(VkPhysicalDevice phys, VkDevice dev) {
    if (!Enabled() && !BudgetDiagEnabled()) return;
    g_phys = phys;
    g_dev = dev;
    if (Enabled()) {
        fprintf(stderr, "[VK-HANG] init diag on: timeout=%us frame-budget-print=30\n", TimeoutSecs());
        PrintMemoryBudget("init");
    } else {
        // R12-3.2: budget-only 模式（PRIMAL_VK_MEM_BUDGET_DIAG=1）。
        fprintf(stderr, "[VK-MEMBUDGET] budget diag on: frame-budget-print=30 (no watchdog, no exit)\n");
        PrintMemoryBudget("init", "[VK-MEMBUDGET]");
    }
}

inline void NoteFrameBegin() {
    if (!Enabled() && !BudgetDiagEnabled()) return;
    const uint64_t f = g_frame.fetch_add(1) + 1;
    if (f % 30 != 0) return;
    if (Enabled()) PrintMemoryBudget("periodic");
    else PrintMemoryBudget("periodic", "[VK-MEMBUDGET]");
}

inline void NoteSubmitOk(uint32_t cmdCount) {
    if (!Enabled()) return;
    g_submits.fetch_add(1);
    g_lastSubmitCmdCount.store(cmdCount);
}

inline void NoteAcquireOk(uint32_t imageIndex) {
    if (!Enabled()) return;
    g_lastAcquireFrame.store(static_cast<int64_t>(g_frame.load()));
    g_lastAcquireImage.store(imageIndex);
}

inline void NotePresentOk(uint32_t imageIndex) {
    if (!Enabled()) return;
    g_lastPresentFrame.store(static_cast<int64_t>(g_frame.load()));
    g_lastPresentImage.store(imageIndex);
}

}  // namespace primal::graphics::rhi::hangdiag

#endif  // ENABLE_VULKAN
#endif  // PRIMAL_GRAPHICS_RHI_VULKAN_HANG_DIAG_H
