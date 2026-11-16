// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Buffer.h — buffer 资源（M1 F1-3）。
//
// 经 D3D12MA 创建 committed 资源。GPUMemoryUsage → D3D12 heap type /
// 初始状态映射（对齐 VulkanBuffer 的 VMA 策略语义）：
//   - Staging / Dynamic → UPLOAD heap，初始 GENERIC_READ（规定值），持久 Map
//     （CPU 写 → GPU 读；GENERIC_READ 含 COPY_SOURCE 位 → 拷贝源零 barrier）
//   - Readback          → READBACK heap，初始 COPY_DEST（规定值），持久 Map
//   - Static / Immutable / 其它 → DEFAULT heap，初始 COMMON（buffer 的
//     COMMON→任意状态 promotion 规则使其读/写使用零显式 barrier，提交边界
//     自动衰减回 COMMON——因此 buffer 不进 D3D12StateTracker）
//   - UAV 路由（R19-1）：DEFAULT 堆 + bindFlags 显式 Storage/
//     UnorderedAccess 位 → 资源 Flags 追加 ALLOW_UNORDERED_ACCESS
//     （CreateUnorderedAccessView 契约）。UAV 标志与 UPLOAD/READBACK 堆
//     互斥故不路由；带标志资源失去 simultaneous-access 隐式提升/衰减，
//     其 UAV 写/拷贝源读取的状态转换由调用方经 StateTracker 显式化。
//   - UAV 宿主堆改道（R16）：bindFlags 显式 UnorderedAccess 位（shader
//     RW 写目标语义）的 buffer 若按 memoryUsage 落在 UPLOAD/READBACK 堆，
//     改道 DEFAULT 堆使 UAV 路由置位——否则 set 写入对宿主堆资源建 UAV
//     视图 = CREATEUNORDEREDACCESSVIEW_INVALIDRESOURCE → 设备移除
//     DXGI_ERROR_INVALID_CALL（Windows CI f350 frame0 三轮复现根因）。
//     持久 Map 随改道失效（消费面已审计为 GPU 写目标）；仅 Storage 位、
//     无显式 UA 位的宿主可见堆 buffer 不改道（SRV-only 消费 + CPU 直填）。
//   - 宿主堆 = 无 barrier 区（R19，run 43887484324）：UPLOAD/READBACK 堆
//     buffer 的规定态 GENERIC_READ/COPY_DEST 是终态，不可被 ResourceBarrier
//     转换——InsertBarrier 对宿主堆 buffer 整条丢弃（tracker 钉 General），
//     CopyBuffer 拦"UPLOAD 作 dst / READBACK 作 src"的不可能拷贝（否则
//     驱动层楔死：fence 永不 signal → watchdog exit 7）。DEFAULT 堆
//     buffer（含 UAV 标志资源）按 buffer 的 COMMON 隐式提升/提交衰减规则
//     走既有零 op/tracker 路径——ALLOW_UNORDERED_ACCESS 标志不剥夺 buffer
//     的隐式提升（该限制属 non-simultaneous-access 纹理；frames 1-3 带
//     标志 culling buffer 的 compute/拷贝路径实证）。
//
// UpdateData 路由：UPLOAD heap → 直接 memcpy；DEFAULT heap → 设备上传环
// （帧内 QueueCopyBuffer 下一命令缓冲编码；帧外 FlushBlocking 阻塞）。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

#include "../../Core/RHIResource.h"

#include "D3D12MemAlloc.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

class D3D12Buffer : public RHIResource {
    friend class primal::graphics::rhi::D3D12Device;
public:
    D3D12Buffer(D3D12Device& device, const BufferDesc& desc);
    D3D12Buffer(D3D12Buffer&& other) noexcept;
    D3D12Buffer& operator=(D3D12Buffer&& other) noexcept;
    D3D12Buffer(const D3D12Buffer&) = delete;
    D3D12Buffer& operator=(const D3D12Buffer&) = delete;
    virtual ~D3D12Buffer();

    bool Initialize() override;

    ID3D12Resource* GetNativeResource() const { return resource_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const {
        return resource_ ? resource_->GetGPUVirtualAddress() : 0;
    }
    D3D12_HEAP_TYPE GetHeapType() const { return heapType_; }
    /// R17：资源是否实际带 ALLOW_UNORDERED_ACCESS 创建。DEFAULT 堆 ≠ 有
    /// 标志（快照/placeholder 类 Static+无 storage 位 buffer 在 DEFAULT 堆
    /// 但无标志）——CreateUnorderedAccessView 写入侧的 fail-soft 判据必须
    /// 用本值而非堆类型（run 43876024964 实测 heapDefault=1 误导排查一轮）。
    bool HasUavFlag() const { return uavFlagged_; }
    const BufferDesc& GetBufferDesc() const { return bufferDesc_; }

protected:
    // === RHIResource 接口 ===
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12MA::Allocation* allocation_ = nullptr;
    void* mappedPtr_ = nullptr;             // UPLOAD/READBACK 持久映射
    D3D12_HEAP_TYPE heapType_ = D3D12_HEAP_TYPE_DEFAULT;
    bool uavFlagged_ = false;               // R17：Initialize 按最终 resDesc.Flags 记录
    bool warnedMapUnsupported_ = false;
    BufferDesc bufferDesc_{};               // 原始 desc（size/usage/类型信息）
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
