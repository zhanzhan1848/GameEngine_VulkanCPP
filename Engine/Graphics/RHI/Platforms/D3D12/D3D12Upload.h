// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Upload.h — 帧槽 UPLOAD staging 环（M1 F1-2）。
//
// 模式对齐 VulkanStagingAllocator（4 frame ring = MAX_FRAMES_IN_FLIGHT + 1
// 安全余量），差异仅在同构 D3D12 语义：
//   - 每帧槽一个 UPLOAD heap 线性 bump buffer（GENERIC_READ + 持久 Map；
//     UPLOAD heap buffer 的 COMMON 提升规则使其无需任何 barrier 即可作
//     CopyBufferRegion / CopyTextureRegion 的源）。
//   - BeginFrame()：轮转到下一槽；复用判定 = 该槽上次被消费的 fence 目标值
//     已完成（D3D12FencePool::GetCompletedValue >= pool.lastUseTarget）——
//     无帧数算术。RenderSystem::BeginFrame 已等过帧槽 fence（正常路径
//     立即通过）；未完成则 CPU 等待（防御旁路提交顺序）。
//   - Allocate(size, align)：当前槽线性 bump；耗尽返回 overflow=true，
//     调用方 fallback 到 FlushBlocking 后重试（Vulkan/Metal 同款约定）。
//   - QueueCopyBuffer / QueueCopyTexture：挂起拷贝列表，帧首
//     EncodePendingCopies(cmdList) 统一编码（用户 pass 之前）。
//   - 纹理上传：区域级 upload heap 布局由 ComputeRegionLayout 计算
//     （RowPitch 对齐 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT=256；源 footprint
//     宽 = 区域宽，CopyTextureRegion 的 BOX 选区）；CopyTextureRegion 以
//     PLACED_FOOTPRINT 源执行；目标纹理的 →COPY_DEST→回置 状态转换经
//     M0 D3D12StateTracker（集成点在 EncodePendingCopies）。
//   - FlushBlocking()：无帧上下文（资产加载期）的一次性提交 +
//     fence 目标值 CPU 等待。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "../../Core/RHITypes.h"
#include "D3D12Abi.h"

// D3D12MemAlloc（v3.0.1，include 路径由 Engine/CMakeLists.txt 的
// WIN32+ENABLE_D3D12 分支注入）。
#include "D3D12MemAlloc.h"

namespace primal::graphics::rhi {

class D3D12Device;

namespace d3d12 {

class D3D12FencePool;

class D3D12UploadRing {
public:
    static constexpr u32 FRAME_SLOTS = MAX_FRAMES_IN_FLIGHT + 1;  // 4
    static constexpr u64 DEFAULT_POOL_SIZE = 16ULL * 1024ULL * 1024ULL;  // 16MB/帧槽
    static constexpr u64 DEFAULT_ALIGNMENT = 256;  // texture copy RowPitch 对齐下限

    struct Allocation {
        void* cpuPtr = nullptr;              ///< 持久映射写入目标
        ID3D12Resource* stagingResource = nullptr;
        u64 offset = 0;
        bool overflow = false;               ///< 容量耗尽：调用方 FlushBlocking + 重试
    };

    D3D12UploadRing() = default;
    ~D3D12UploadRing();
    D3D12UploadRing(const D3D12UploadRing&) = delete;
    D3D12UploadRing& operator=(const D3D12UploadRing&) = delete;

    /// upload heap 区域布局（调用方据此 Allocate）：
    /// footprint 按区域宽 × bytesPerPixel 计算（D3D12 允许源 footprint 宽
    /// 小于目标子资源宽——CopyTextureRegion 的 BOX 选区），RowPitch 对齐
    /// D3D12_TEXTURE_DATA_PITCH_ALIGNMENT=256；totalBytes 含行距 padding
    /// （末行不 pad、深度逐层完整 pad）。
    struct TextureUploadLayout {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u64 totalBytes = 0;
        u64 rowPitch = 0;
    };
    static TextureUploadLayout ComputeRegionLayout(u32 dxgiFormat, u32 bytesPerPixel,
                                                   u32 width, u32 height, u32 depth,
                                                   u64 baseOffset);

    void Initialize(D3D12Device* device, u64 poolSize = DEFAULT_POOL_SIZE);
    void Shutdown();

    /// 帧槽推进（设备 beginFrameImpl 调用）：轮转 + 复用判定 + bump 归零。
    void BeginFrame();

    /// 当前帧槽线性分配。
    Allocation Allocate(u64 size, u64 alignment = DEFAULT_ALIGNMENT);

    /// 挂起 buffer→buffer 拷贝（帧首统一编码）。
    void QueueCopyBuffer(const Allocation& alloc, ID3D12Resource* dst, u64 dstOffset,
                         u64 size);

    /// 挂起 buffer→texture 拷贝（单子资源区域）。
    /// layout 由 ComputeRegionLayout 预算（与 alloc 的 offset/size 对应）；
    /// subresource 为 D3D12 线性子资源索引（mip + mipLevels * arraySlice）；
    /// backState 为拷贝完成后纹理的回置状态（通常 ShaderResource）。
    void QueueCopyTexture(const Allocation& alloc, ID3D12Resource* dstTexture,
                          u64 textureId, u32 subresource,
                          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,
                          u32 offsetX, u32 offsetY, u32 offsetZ,
                          u32 width, u32 height, u32 depth,
                          ResourceState backState);

    /// 帧首把当前槽挂起拷贝编码进命令列表（D3D12CommandBuffer::Begin 调用）。
    void EncodePendingCopies(ID3D12GraphicsCommandList* cmd);

    /// 设备 Submit 时登记：当前槽的分配已被该 fence 目标值消费（复用判定）。
    void OnSubmitConsumed(u64 fenceTarget) {
        pools_[currentSlot_].lastUseTarget = fenceTarget;
    }

    /// 无帧上下文的阻塞 flush（资产加载期）：一次性命令列表 + fence 等待。
    void FlushBlocking();

    bool HasPendingCopies() const { return !pools_[currentSlot_].pending.empty(); }
    bool IsInitialized() const { return device_ != nullptr; }

private:
    struct PendingCopy {
        ID3D12Resource* srcStaging = nullptr;  // ring buffer 本体（非拥有）
        u64 srcOffset = 0;

        bool isTexture = false;
        // buffer 目标
        ID3D12Resource* dstBuffer = nullptr;
        u64 dstBufferOffset = 0;
        u64 size = 0;
        // texture 目标
        ID3D12Resource* dstTexture = nullptr;
        u64 textureId = 0;  // StateTracker 的 resource id（物理句柄数值）
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        u32 subresource = 0;
        UINT copyX = 0, copyY = 0, copyZ = 0;
        UINT copyW = 0, copyH = 0, copyD = 0;
        ResourceState backState = ResourceState::ShaderResource;
    };

    struct FramePool {
        ID3D12Resource* buffer = nullptr;      // D3D12MA 分配（Shutdown 统一释放）
        D3D12MA::Allocation* allocation = nullptr;
        void* mappedPtr = nullptr;
        u64 offset = 0;                        // 当前 bump
        u64 capacity = 0;
        u64 lastUseTarget = 0;                 ///< 上次消费提交的 fence 目标值
        std::vector<PendingCopy> pending;
    };

    /// 设备侧编码辅助（StateTracker 集成 + 提交）；供 EncodePendingCopies 与
    /// FlushBlocking 复用。
    void EncodePoolCopies(ID3D12GraphicsCommandList* cmd, FramePool& pool);

    D3D12Device* device_ = nullptr;   // 非拥有；访问 allocator/fence/tracker
    FramePool pools_[FRAME_SLOTS];
    u32 currentSlot_ = 0;
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
