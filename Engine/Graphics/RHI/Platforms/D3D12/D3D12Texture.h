// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Texture.h — 纹理资源（M1 F1-3）。
//
// 形态：ID3D12Resource（TEXTURE1D/2D/3D，经 D3D12MA committed）+
// D3D12Format::ToDxgiFormat 的 DXGI 格式 + D3D12StateTracker 的
// per-mip/per-subresource 注册（device createTextureImpl 完成）。
//
// 描述符策略（view 信息折叠进描述符——Parity Matrix 决策）：
//   - usage 含 RenderTarget  → 创建时即分配 RTV 槽 + CreateRenderTargetView
//   - usage 含 DepthStencil  → DSV 槽 + CreateDepthStencilView
//   - usage 含 ShaderResource→ SRV 槽（shader-visible 主堆）+ CreateShaderResourceView
//   - usage 含 UnorderedAccess→ UAV 槽 + CreateUnorderedAccessView
//   槽位持有者为本对象；释放经 destroyImpl（GC 延迟）。
//
// 构造模式（对齐 VulkanTexture 三构造）：
//   - 常规：D3D12MA 分配 + 创建，拥有资源与分配；
//   - wrap：包裹外部 ID3D12Resource（交换链 backbuffer；ComPtr 持引用，
//     交换链销毁前先销毁 wrap 层）；
//   - view：包裹源纹理资源 + 受限 mip/array 范围的 SRV（M1 仅 SRV 视图；
//     RTV/DSV 视图 NOT-IMPLEMENTED，显式日志）。
//
// 多平面：D32_Float_S8X24 = 2 plane（深度/模板）。M1 支持 plane0（深度）
// 的 SRV/DSV；模板面访问 NOT-IMPLEMENTED（登记 M2+）。
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

class D3D12Texture : public RHIResource {
    friend class primal::graphics::rhi::D3D12Device;
public:
    /// 常规构造（D3D12MA CreateResource）。
    D3D12Texture(D3D12Device& device, const TextureDesc& desc);

    /// wrap 构造（交换链 backbuffer；不拥有 D3D12MA 分配）。
    D3D12Texture(D3D12Device& device, const TextureDesc& desc, ID3D12Resource* existing);

    /// view 构造（createTextureViewImpl；受限 SRV）。
    D3D12Texture(D3D12Device& device, const TextureViewDesc& viewDesc, D3D12Texture& src);

    D3D12Texture(D3D12Texture&& other) noexcept;
    D3D12Texture& operator=(D3D12Texture&& other) noexcept;
    D3D12Texture(const D3D12Texture&) = delete;
    D3D12Texture& operator=(const D3D12Texture&) = delete;
    virtual ~D3D12Texture();

    bool Initialize() override;

    ID3D12Resource* GetNativeResource() const { return resource_.Get(); }
    const TextureDesc& GetTextureDesc() const { return texDesc_; }

    /// 资源描述（footprint / CopyTextureRegion / GenerateMipmaps 类路径用）。
    D3D12_RESOURCE_DESC GetNativeDesc() const {
        return resource_ ? resource_->GetDesc() : D3D12_RESOURCE_DESC{};
    }

    bool IsWrapMode() const { return wrapMode_; }
    bool IsViewMode() const { return viewMode_; }
    u32 PlaneCount() const { return planeCount_; }
    u32 SubresourceCount() const {
        return texDesc_.mipLevels * texDesc_.arraySize * planeCount_;
    }

    /// 每 texel 字节数（非压缩格式；压缩格式返回 0）。拷贝 footprint 与
    /// 上传路径共用的布局换算依据。
    u64 BytesPerTexel() const;

    // 描述符槽位（kInvalidDescriptorSlot = 未创建）
    static constexpr u32 kInvalidDescriptorSlot = 0xFFFFFFFFu;
    u32 SrvSlot() const { return srvSlot_; }
    u32 UavSlot() const { return uavSlot_; }
    u32 RtvSlot() const { return rtvSlot_; }
    u32 DsvSlot() const { return dsvSlot_; }

    /// VT M0.7 语义的子区域上传（2D 单 slice、任意 mip、非压缩格式）。
    bool updateRegion(const void* data, u64 size, const TextureRegionUpdate& region);

protected:
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    bool createDescriptors();  // Initialize 尾部：按 usage 建 RTV/DSV/SRV/UAV

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    D3D12MA::Allocation* allocation_ = nullptr;  // wrap/view 模式 = nullptr

    bool wrapMode_ = false;
    bool viewMode_ = false;
    TextureViewDesc viewDesc_{};   // view 模式的受限范围

    u32 planeCount_ = 1;
    u32 srvSlot_ = kInvalidDescriptorSlot;
    u32 uavSlot_ = kInvalidDescriptorSlot;
    u32 rtvSlot_ = kInvalidDescriptorSlot;
    u32 dsvSlot_ = kInvalidDescriptorSlot;

    TextureDesc texDesc_{};
};

} // namespace primal::graphics::rhi::d3d12
} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
