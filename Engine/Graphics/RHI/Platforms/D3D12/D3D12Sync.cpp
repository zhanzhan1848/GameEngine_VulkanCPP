// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Sync.cpp — SyncHandle 承载体实现（M1）。
#include "D3D12Sync.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <windows.h>

namespace primal::graphics::rhi::d3d12 {

D3D12SyncObject::~D3D12SyncObject() { Shutdown(); }

bool D3D12SyncObject::Initialize(ID3D12Device* device) {
    if (!device) return false;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        return false;
    }
    event_ = ::CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!event_) {
        fence_.Reset();
        return false;
    }
    lastSignalValue_ = 0;
    return true;
}

void D3D12SyncObject::Shutdown() {
    if (event_) {
        ::CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
    }
    fence_.Reset();
    lastSignalValue_ = 0;
}

u64 D3D12SyncObject::CompletedValue() const {
    return fence_ ? static_cast<u64>(fence_->GetCompletedValue()) : 0;
}

bool D3D12SyncObject::Wait(u64 target, u32 timeoutMs) {
    if (!fence_ || !event_) return false;
    if (fence_->GetCompletedValue() >= target) return true;
    if (FAILED(fence_->SetEventOnCompletion(target, static_cast<HANDLE>(event_)))) {
        return false;
    }
    return ::WaitForSingleObject(static_cast<HANDLE>(event_), timeoutMs) == WAIT_OBJECT_0;
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
