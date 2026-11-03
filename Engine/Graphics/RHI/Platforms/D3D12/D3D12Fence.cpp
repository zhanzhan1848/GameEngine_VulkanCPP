// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Fence.cpp — fence 目标值 / GPU 完成值查询实现（M0 F0-8；M1 补
// PeekNextTarget 供分帧槽护拦登记）。
#include "D3D12Fence.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

namespace primal::graphics::rhi::d3d12 {

bool D3D12FencePool::Initialize(ID3D12Device* device, u64 initialValue) {
    if (!device) return false;
    if (FAILED(device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence_)))) {
        return false;
    }
    event_ = ::CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!event_) {
        fence_.Reset();
        return false;
    }
    signaled_ = initialValue;
    deviceRemoved_ = false;
    return true;
}

void D3D12FencePool::Shutdown() {
    if (event_) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    fence_.Reset();
    signaled_ = 0;
}

u64 D3D12FencePool::NextTarget() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++signaled_;
}

u64 D3D12FencePool::PeekNextTarget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_ + 1;
}

bool D3D12FencePool::Signal(ID3D12CommandQueue* queue, u64 target) {
    if (!queue || !fence_) return false;
    return SUCCEEDED(queue->Signal(fence_.Get(), target));
}

u64 D3D12FencePool::GetCompletedValue() const {
    return fence_ ? fence_->GetCompletedValue() : 0;
}

bool D3D12FencePool::Wait(u64 target, u32 timeoutMs) {
    if (!fence_ || !event_) return false;
    if (deviceRemoved_) return false;  // 语义：等待已无意义，调用方走丢失路径
    if (fence_->GetCompletedValue() >= target) return true;
    if (FAILED(fence_->SetEventOnCompletion(target, event_))) return false;
    return ::WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
