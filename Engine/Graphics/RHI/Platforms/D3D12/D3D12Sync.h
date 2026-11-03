// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Sync.h — SyncHandle 承载体（M1：Submit/WaitForSync/BeginFrame 帧循环）。
//
// RHI 同步契约映射（VulkanSync = VkFence + VkSemaphore 的 D3D12 对等）：
//   - SyncHandle（QueueSubmitInfo::signalFence / RenderSystem frameFences_）→
//     本对象 = 单个 ID3D12Fence + 单调目标值。Vulkan fence 的
//     reset-then-signal 循环被"单调递增目标值"取代（D3D12 fence 天然支持），
//     因此没有 reset 竞态：初始 completedValue=0 且 lastSignal_=0，
//     首帧 WaitForSync 立即通过（对齐 Vulkan createSyncImpl 的"初始
//     signaled"注释语义）。
//   - Semaphore 语义（waitSemaphore/signalSemaphore/AcquireNextImage 的
//     imageAvailable）：D3D12 无 GPU-GPU semaphore；DIRECT 单队列上
//     ExecuteCommandLists 的顺序即同步（Submit/Present/Acquire 的顺序依赖
//     由队列序保证）。相关参数在设备层"接受并忽略"（一次性日志）。
//   - CPU 等待：GetCompletedValue >= target 短路；否则
//     SetEventOnCompletion + WaitForSingleObject（超时语义与
//     RHIDeviceBase::WaitForSync 的 ms 口径一致）。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace primal::graphics::rhi::d3d12 {

class D3D12SyncObject {
public:
    D3D12SyncObject() = default;
    ~D3D12SyncObject();
    D3D12SyncObject(const D3D12SyncObject&) = delete;
    D3D12SyncObject& operator=(const D3D12SyncObject&) = delete;

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    ID3D12Fence* GetFence() const { return fence_.Get(); }

    /// 提交前调用：目标值 = 上次 + 1（不 Signal；Signal 由设备 Submit 统一执行）。
    u64 NextSignalValue() { return ++lastSignalValue_; }
    u64 LastSignalValue() const { return lastSignalValue_; }

    /// GPU 完成值查询（非阻塞）。
    u64 CompletedValue() const;

    /// CPU 等待到目标值（事件等待；超时 false）。
    bool Wait(u64 target, u32 timeoutMs);

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    void* event_ = nullptr;  // HANDLE（void* 避免 windows.h 泄漏到本头）
    u64 lastSignalValue_ = 0;
};

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
