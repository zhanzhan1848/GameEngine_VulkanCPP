// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Fence.h — fence 目标值 / GPU 完成值查询（M0 F0-8）。
// 契约（阶段文档 §3.4）：Submit 记录 fence 目标值；allocator reset /
// descriptor 回收 / staging 复用查询 GetCompletedValue——生命周期以 fence
// 值为据，不做帧数算术。本类不跟踪"哪帧到哪了"，只提供目标值登记与
// 完成值查询；帧槽位语义归 FrameSlotCompletion（RHI Core）。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <cstdint>
#include <mutex>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

namespace primal::graphics::rhi::d3d12 {

class D3D12FencePool {
public:
    D3D12FencePool() = default;
    ~D3D12FencePool() { Shutdown(); }

    bool Initialize(ID3D12Device* device, u64 initialValue = 0);
    void Shutdown();

    /// 提交时登记：目标值 = 当前已发信号 + 1，返回该目标值给调用方登记到
    /// 对应命令提交。线程安全（RenderSystem 提交路径单线程，但 staging /
    /// readback 旁路可能并发 Signal）。
    u64 NextTarget();

    /// 预读下一个将被登记的目标值（不递增）——帧内分配（描述符槽/staging）
    /// 的护拦登记用：消费该分配的提交即将 Signal 此值。
    /// M1 新增（F1-5 分帧槽护拦）；与 NextTarget 同锁序。
    u64 PeekNextTarget() const;

    /// 在 DIRECT queue 上发出 Signal(fence, target)。
    bool Signal(ID3D12CommandQueue* queue, u64 target);

    /// GPU 完成值查询（非阻塞）。
    u64 GetCompletedValue() const;

    /// CPU 等待到目标值（事件等待；timeoutMs 超时返回 false）。
    bool Wait(u64 target, u32 timeoutMs);

    /// 设备丢失后 GetCompletedValue 可能恒为旧值——Wait 先检查 removed reason。
    void OnDeviceRemoved() { deviceRemoved_ = true; }

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    u64 signaled_ = 0;          // 已登记的最近目标值
    bool deviceRemoved_ = false;
    std::mutex mutex_;
};

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
