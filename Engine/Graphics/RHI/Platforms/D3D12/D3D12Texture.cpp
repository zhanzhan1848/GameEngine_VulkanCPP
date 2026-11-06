// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Texture.cpp — 纹理资源实现（M1 F1-3）。
#include "D3D12Texture.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <cstring>

#include "D3D12DescriptorHeaps.h"
#include "D3D12Device.h"
#include "D3D12Format.h"
#include "D3D12Upload.h"

#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi::d3d12 {

namespace {

/// DataFormat → 每 texel 字节数（非压缩格式；压缩返回 0 = 走命令层路径）。
/// 覆盖 RHI DataFormat 的非压缩家族（与 D3D12Format::ToDxgiFormat 的
/// 支持域一致）。
u64 DataFormatBytesPerTexel(DataFormat format) {
    switch (format) {
        case DataFormat::R8_UNorm: case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:  case DataFormat::R8_SInt:
            return 1;
        case DataFormat::R16_UNorm: case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt:  case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
        case DataFormat::RG8_UNorm: case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt:  case DataFormat::RG8_SInt:
            return 2;
        case DataFormat::R32_UNorm: case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt:  case DataFormat::R32_SInt:
        case DataFormat::R32_Float:
        case DataFormat::RG16_UNorm: case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt:  case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
        case DataFormat::BGRA8_UNorm: case DataFormat::BGRA8_UInt:
        case DataFormat::BGRA8_SInt:
        case DataFormat::RG8B8A8_UNorm: case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt:  case DataFormat::RG8B8A8_SInt:
        case DataFormat::RGBA8_UNorm: case DataFormat::RGBA8_SNorm:
        case DataFormat::RGBA8_UInt:  case DataFormat::RGBA8_SInt:
        case DataFormat::RGBA8_sRGB:
        case DataFormat::D32_Float:
        case DataFormat::D24_UNorm_S8_UInt:   // plane0 深度 4B（模板在 plane1）
        case DataFormat::D32_Float_S8X24_UInt:
            return 4;
        case DataFormat::R8G8B8_UNorm: case DataFormat::R8G8B8_SNorm:
        case DataFormat::R8G8B8_UInt:  case DataFormat::R8G8B8_SInt:
            return 3;  // 注：DXGI 无 24-bit 布局（ToDxgiFormat 返回 Unknown）
        case DataFormat::RG32_UNorm: case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt:  case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
        case DataFormat::RGB32_UNorm: case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt:  case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
        case DataFormat::RGBA16_UNorm: case DataFormat::RGBA16_SNorm:
        case DataFormat::RGBA16_UInt:  case DataFormat::RGBA16_SInt:
        case DataFormat::RGBA16_Float:
            return 8;
        case DataFormat::RGBA32_UNorm: case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt:  case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
            return 16;
        default:
            return 0;  // BC/ASTC 压缩家族 + Unknown
    }
}

/// RHI TextureType → D3D12_RESOURCE_DIMENSION。
D3D12_RESOURCE_DIMENSION ToResourceDimension(TextureType type) {
    switch (type) {
        case TextureType::Texture1D:
        case TextureType::Texture1DArray: return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        case TextureType::Texture3D:      return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        case TextureType::Texture2D:
        case TextureType::Texture2DArray:
        case TextureType::TextureCube:
        case TextureType::TextureCubeArray:
        default:                          return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }
}

/// M6 F6-2/F6-3：typed UAV 附加格式需求判定。D3D12 只保证 R32_FLOAT/
/// R32_UINT/R32_SINT 三格式的 typed UAV load/store；RGBA16F/RG16F 等
/// （GI 探针网格/SPGI 级联等 3D 体积载体的主格式）需设备
/// TypedUAVLoadAdditionalFormats 能力（D3D12Device::GetTypedUavAdditional
/// Formats，OPTIONS3 真实查询）。能力不足时 UAV 创建显式失败（fail 语义，
/// 不降级不静默——M6 阶段文档 §3 契约）。
bool FormatNeedsTypedUavAdditionalFormats(DataFormat format) {
    switch (format) {
        case DataFormat::R32_Float:
        case DataFormat::R32_UInt:
        case DataFormat::R32_SInt:
            return false;   // D3D12 保证集
        default:
            return true;    // 保守：其余全走附加格式能力门
    }
}

/// 深度家族的 SRV 采样格式（D 变体不可作 SRV 格式——按平面取中间格式）：
///   D32_Float → R32_Float；D16_UNorm → R16_UNorm（均单平面）；
///   D24S8/D32S8 → plane0 typeless 中间格式（双平面，M1 仅深度面）。
u32 DepthSrvFormat(DataFormat format) {
    switch (format) {
        case DataFormat::D32_Float:            return kDXGIFormat_R32_Float;
        case DataFormat::D16_UNorm:            return kDXGIFormat_R16_UNorm;
        case DataFormat::D32_Float_S8X24_UInt: return kDXGIFormat_R32_Float_X8X24_Typeless;  // 21
        case DataFormat::D24_UNorm_S8_UInt:    return kDXGIFormat_R24_UNorm_X8_Typeless;     // 46
        default:                               return kDXGIFormat_Unknown;
    }
}

} // namespace

// ============================================================================
// 构造 / 生命周期
// ============================================================================

D3D12Texture::D3D12Texture(D3D12Device& device, const TextureDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Texture, ResourceUsage::None,
                                       desc.memoryUsage, 0, desc.name.c_str())),
      texDesc_(desc) {}

D3D12Texture::D3D12Texture(D3D12Device& device, const TextureDesc& desc,
                           ID3D12Resource* existing)
    : RHIResource(device, ResourceDesc(ResourceType::Texture, ResourceUsage::None,
                                       desc.memoryUsage, 0, desc.name.c_str())),
      wrapMode_(true), texDesc_(desc) {
    resource_ = existing;  // ComPtr AddRef：wrap 层存活期与交换链销毁顺序由
                           // destroySwapChainImpl 保证（先销毁 wrap 再销毁交换链）。
}

D3D12Texture::D3D12Texture(D3D12Device& device, const TextureViewDesc& viewDesc,
                           D3D12Texture& src)
    : RHIResource(device, ResourceDesc(ResourceType::Texture, ResourceUsage::None,
                                       GPUMemoryUsage::Unknown, 0, nullptr)),
      viewMode_(true), viewDesc_(viewDesc) {
    resource_ = src.resource_;       // 别名源资源（不拥有分配）
    texDesc_ = src.texDesc_;
}

D3D12Texture::D3D12Texture(D3D12Texture&& other) noexcept
    : RHIResource(std::move(other)),
      resource_(std::move(other.resource_)),
      allocation_(other.allocation_),
      wrapMode_(other.wrapMode_),
      viewMode_(other.viewMode_),
      viewDesc_(other.viewDesc_),
      planeCount_(other.planeCount_),
      srvSlot_(other.srvSlot_),
      uavSlot_(other.uavSlot_),
      rtvSlot_(other.rtvSlot_),
      dsvSlot_(other.dsvSlot_),
      texDesc_(std::move(other.texDesc_)) {
    other.allocation_ = nullptr;
    other.srvSlot_ = kInvalidDescriptorSlot;
    other.uavSlot_ = kInvalidDescriptorSlot;
    other.rtvSlot_ = kInvalidDescriptorSlot;
    other.dsvSlot_ = kInvalidDescriptorSlot;
}

D3D12Texture& D3D12Texture::operator=(D3D12Texture&& other) noexcept {
    if (this != &other) {
        RHIResource::operator=(std::move(other));
        resource_ = std::move(other.resource_);
        allocation_ = other.allocation_;
        wrapMode_ = other.wrapMode_;
        viewMode_ = other.viewMode_;
        viewDesc_ = other.viewDesc_;
        planeCount_ = other.planeCount_;
        srvSlot_ = other.srvSlot_;
        uavSlot_ = other.uavSlot_;
        rtvSlot_ = other.rtvSlot_;
        dsvSlot_ = other.dsvSlot_;
        texDesc_ = std::move(other.texDesc_);
        other.allocation_ = nullptr;
        other.srvSlot_ = kInvalidDescriptorSlot;
        other.uavSlot_ = kInvalidDescriptorSlot;
        other.rtvSlot_ = kInvalidDescriptorSlot;
        other.dsvSlot_ = kInvalidDescriptorSlot;
    }
    return *this;
}

D3D12Texture::~D3D12Texture() { destroyImpl(); }

bool D3D12Texture::Initialize() {
    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    ID3D12Device* native = dev.GetNativeDevice();
    if (!native) return false;

    // 平面数：含模板位的深度格式为双平面（以 TYPELESS 父格式创建）；
    // 纯深度（D32_Float/D16_UNorm）单平面。
    planeCount_ = (texDesc_.format == DataFormat::D32_Float_S8X24_UInt
                   || texDesc_.format == DataFormat::D24_UNorm_S8_UInt) ? 2 : 1;
    const bool isDepthFamily = planeCount_ == 2
        || texDesc_.format == DataFormat::D32_Float
        || texDesc_.format == DataFormat::D16_UNorm;

    if (!wrapMode_ && !viewMode_) {
        // 常规路径：D3D12MA committed 创建。
        D3D12MA::Allocator* allocator = dev.GetD3D12MAAllocator();
        if (!allocator) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[D3D12Texture] D3D12MA allocator missing" << std::endl;
            return false;
        }

        u32 dxgi = ToDxgiFormat(texDesc_.format);
        // 深度家族：资源本体用 TYPELESS 父格式，视图（DSV/SRV）分别带具体
        // 格式。双平面是 D3D12 平面资源规则；单平面纯深度在需挂 SRV 时同样
        // 必须 TYPELESS——typed D32_FLOAT 资源上建 R32_FLOAT SRV 视图不兼容
        // （SampleCmp 恒 0，M2 阴影≡0 根因）；DSV-only 深度保持 typed 创建。
        const bool depthSrvCast = isDepthFamily
            && (texDesc_.usage & TextureUsage::ShaderResource) != TextureUsage::Unknown;
        if (planeCount_ == 2 || depthSrvCast) {
            dxgi = GetTypelessDxgiFormat(texDesc_.format);
            if (dxgi == kDXGIFormat_Unknown) {
                PRIMAL_LOG_STREAM(Error, "RHI")
                    << "[D3D12Texture] depth-stencil format has no typeless parent: "
                    << static_cast<u32>(texDesc_.format) << std::endl;
                return false;
            }
        }
        if (dxgi == kDXGIFormat_Unknown) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Texture] unsupported format "
                << static_cast<u32>(texDesc_.format) << " name='" << texDesc_.name << "'" << std::endl;
            return false;  // 显式不支持（ASTC 家族等，见 D3D12Format 契约）
        }

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension = ToResourceDimension(texDesc_.type);
        resDesc.Alignment = 0;
        resDesc.Width = texDesc_.size.x;
        resDesc.Height = texDesc_.type == TextureType::Texture1D
                             || texDesc_.type == TextureType::Texture1DArray ? 1 : texDesc_.size.y;
        if (resDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            resDesc.DepthOrArraySize = static_cast<UINT16>(texDesc_.size.z);
        } else {
            resDesc.DepthOrArraySize = static_cast<UINT16>(texDesc_.arraySize);
        }
        resDesc.MipLevels = static_cast<UINT16>(texDesc_.mipLevels);
        resDesc.Format = static_cast<DXGI_FORMAT>(dxgi);
        resDesc.SampleDesc.Count = 1;  // M1 无 MSAA
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if ((texDesc_.usage & TextureUsage::RenderTarget) != TextureUsage::Unknown) {
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if ((texDesc_.usage & TextureUsage::DepthStencil) != TextureUsage::Unknown) {
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if ((texDesc_.usage & TextureUsage::UnorderedAccess) != TextureUsage::Unknown) {
            // M6：附加格式能力门（RGBA16F typed UAV store/load 需
            // TypedUAVLoadAdditionalFormats；能力不足显式失败——见上注）。
            if (FormatNeedsTypedUavAdditionalFormats(texDesc_.format)
                && !dev.GetTypedUavAdditionalFormats()) {
                PRIMAL_LOG_STREAM(Error, "RHI")
                    << "[D3D12Texture] typed UAV format " << u32(texDesc_.format)
                    << " requires TypedUAVLoadAdditionalFormats — device does "
                       "not support it (explicit failure, no fallback) name='"
                    << texDesc_.name << "'" << std::endl;
                return false;
            }
            resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        // R15-1 层1：GenerateMipmaps 服务承诺的创建期标志增补。Vulkan 侧
        // ToVkImageUsageFlags（VulkanMath.h）对含 ShaderResource/RenderTarget
        // 的 usage 隐式补 TRANSFER_SRC/DST，blit 链 mipgen 因此恒可用；D3D12
        // mipgen 走 PS downsample 需 RTV，而资源缺 ALLOW_RENDER_TARGET 时
        // mipgen 的 CreateRenderTargetView 触发 debug-layer #42
        // (CREATERENDERTARGETVIEW_INVALIDRESOURCE；Windows 十四测 exit 2170
        // 首帧阻塞，CDB 实证)。故对 mips>1 且含 ShaderResource 的纹理，以与
        // mipgen 入口同口径的 CheckFeatureSupport(RENDER_TARGET 位) 判定格式
        // 能力后增补 RT 标志（深度/BC/TYPELESS 格式无该位、天然不增补）。
        // 合法性：D3D12 资源标志仅 DENY_SHADER_RESOURCE 与 RT/DS 互斥
        // (d3d12.h 注释)，RT 标志与 SRV/COPY/UAV 标志无互斥；增补不改 RHI
        // usage 语义，描述符仍按 usage 创建（无 RenderTarget 位 → 不分配
        // RTV 槽，mipgen 自取临时 RTV 槽）。
        if (texDesc_.mipLevels > 1
            && (texDesc_.usage & TextureUsage::ShaderResource) != TextureUsage::Unknown
            && !isDepthFamily) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT mipRtvSupport{};
            mipRtvSupport.Format = static_cast<DXGI_FORMAT>(dxgi);
            if (SUCCEEDED(native->CheckFeatureSupport(
                    D3D12_FEATURE_FORMAT_SUPPORT, &mipRtvSupport,
                    sizeof(mipRtvSupport)))
                && (mipRtvSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0) {
                resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
        }

        // 初始状态：COMMON（纹理写类使用必须显式 transition——COMMON 只能
        // 自动提升到读类状态；提升/衰减规则见 D3D12StateTracker 文件头）。
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;  // M1 纹理恒 DEFAULT heap
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        HRESULT hr = allocator->CreateResource(
            &allocDesc, &resDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            &allocation_, IID_PPV_ARGS(&resource));
        if (FAILED(hr) || !resource) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[D3D12Texture] CreateResource failed hr=0x" << std::hex
                << static_cast<unsigned long>(hr) << std::dec << " name='" << texDesc_.name
                << "'" << std::endl;
            allocation_ = nullptr;
            return false;
        }
        resource_ = std::move(resource);
    }

    if (!createDescriptors()) return false;
    state_ = ResourceState::Ready;
    return true;
}

bool D3D12Texture::createDescriptors() {    D3D12Device& dev = static_cast<D3D12Device&>(device_);
    ID3D12Device* native = dev.GetNativeDevice();
    D3D12DescriptorHeapManager& heaps = dev.GetDescriptorHeaps();
    if (!native || !resource_) return false;

    const u32 dxgiFull = ToDxgiFormat(texDesc_.format);
    // [R6/Windows] isDepthFamily 原是 Initialize()（L201）的局部量，本函数
    // 不可见（C2065）。在此重算同一表达式（planeCount_ 是成员，成员初始化
    // 于 Initialize 先行，此处读到的即最终值）。
    const bool isDepthFamily = planeCount_ == 2
        || texDesc_.format == DataFormat::D32_Float
        || texDesc_.format == DataFormat::D16_UNorm;

    // ---- RTV（RenderTarget usage；wrap backbuffer 走同一路径） ----
    const bool wantRtv = viewMode_
        ? false  // M1：RTV 视图 NOT-IMPLEMENTED（登记）
        : (texDesc_.usage & TextureUsage::RenderTarget) != TextureUsage::Unknown;
    if (wantRtv) {
        rtvSlot_ = heaps.AllocRtv();
        if (rtvSlot_ == D3D12DescriptorHeapManager::kInvalidSlot) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFull);
        // M6 F6-3：3D RTV 维度（D3D12_TEXTURE3D 拓扑 mip0/全深度切片；
        // GI 切片无 3D RT 消费，视图补全为静态完备性——错配 2D 视图在
        // 3D 资源上是 debug-layer 必错项）。
        if (texDesc_.type == TextureType::Texture3D) {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            rtvDesc.Texture3D.MipSlice = 0;
            rtvDesc.Texture3D.FirstWSlice = 0;
            rtvDesc.Texture3D.WSize = texDesc_.size.z;
        } else {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = 0;
            rtvDesc.Texture2D.PlaneSlice = 0;
        }
        native->CreateRenderTargetView(resource_.Get(), &rtvDesc, heaps.RtvCpuHandle(rtvSlot_));
    }

    // ---- DSV（DepthStencil usage） ----
    const bool wantDsv = viewMode_
        ? false
        : (texDesc_.usage & TextureUsage::DepthStencil) != TextureUsage::Unknown;
    if (wantDsv) {
        dsvSlot_ = heaps.AllocDsv();
        if (dsvSlot_ == D3D12DescriptorHeapManager::kInvalidSlot) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFull);  // D 变体格式
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2D.MipSlice = 0;
        native->CreateDepthStencilView(resource_.Get(), &dsvDesc, heaps.DsvCpuHandle(dsvSlot_));
    }

    // ---- SRV（ShaderResource usage；view 模式带受限范围） ----
    const bool wantSrv = (texDesc_.usage & TextureUsage::ShaderResource) != TextureUsage::Unknown
                         || viewMode_;
    if (wantSrv) {
        // 持久槽位（长寿命纹理——分帧线性区会在 3 帧后轮转覆写，不可用）；
        // set 实例在 UpdateDescriptorSets 时经 CopyDescriptorsSimple 拷入。
        srvSlot_ = heaps.AllocCbvSrvUavPersistent();
        if (srvSlot_ == D3D12DescriptorHeapManager::kInvalidSlot) return false;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        // 深度家族 SRV：D 变体不可作 SRV 格式——按平面取中间格式；
        // sRGB 采样走 ToSrgbDxgiFormat（M1 离屏路径直读 UNorm；sRGB 视图
        // 切换留 M7 呈现验收）。
        u32 srvFormat = isDepthFamily ? DepthSrvFormat(texDesc_.format) : dxgiFull;
        srvDesc.Format = static_cast<DXGI_FORMAT>(srvFormat);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        switch (texDesc_.type) {
            case TextureType::Texture1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = viewMode_ ? viewDesc_.mostDetailedMip : 0;
                srvDesc.Texture1D.MipLevels = viewMode_ ? viewDesc_.mipCount : texDesc_.mipLevels;
                break;
            case TextureType::Texture1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = 0;
                srvDesc.Texture1DArray.MipLevels = texDesc_.mipLevels;
                srvDesc.Texture1DArray.FirstArraySlice = viewMode_ ? viewDesc_.firstArraySlice : 0;
                srvDesc.Texture1DArray.ArraySize = viewMode_ ? viewDesc_.arraySize : texDesc_.arraySize;
                break;
            case TextureType::Texture3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = viewMode_ ? viewDesc_.mostDetailedMip : 0;
                srvDesc.Texture3D.MipLevels = viewMode_ ? viewDesc_.mipCount : texDesc_.mipLevels;
                break;
            case TextureType::TextureCube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = viewMode_ ? viewDesc_.mostDetailedMip : 0;
                srvDesc.TextureCube.MipLevels = viewMode_ ? viewDesc_.mipCount : texDesc_.mipLevels;
                break;
            case TextureType::TextureCubeArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = 0;
                srvDesc.TextureCubeArray.MipLevels = texDesc_.mipLevels;
                srvDesc.TextureCubeArray.First2DArrayFace = 0;
                srvDesc.TextureCubeArray.NumCubes = texDesc_.arraySize / 6;
                break;
            case TextureType::Texture2DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = viewMode_ ? viewDesc_.mostDetailedMip : 0;
                srvDesc.Texture2DArray.MipLevels = viewMode_ ? viewDesc_.mipCount : texDesc_.mipLevels;
                srvDesc.Texture2DArray.FirstArraySlice = viewMode_ ? viewDesc_.firstArraySlice : 0;
                srvDesc.Texture2DArray.ArraySize = viewMode_ ? viewDesc_.arraySize : texDesc_.arraySize;
                srvDesc.Texture2DArray.PlaneSlice = 0;
                // [R6/Windows] 真名 ResourceMinLODClamp（官方头 D3D12_TEX2D_ARRAY_SRV；
                // ResourceMinLOD 不存在）。
                srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
                break;
            case TextureType::Texture2D:
            case TextureType::Unknown:
            default:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = viewMode_ ? viewDesc_.mostDetailedMip : 0;
                srvDesc.Texture2D.MipLevels = viewMode_ ? viewDesc_.mipCount : texDesc_.mipLevels;
                srvDesc.Texture2D.PlaneSlice = 0;
                // [R6/Windows] 真名 ResourceMinLODClamp（官方头 D3D12_TEX2D_SRV）。
                srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
                break;
        }
        native->CreateShaderResourceView(resource_.Get(), &srvDesc,
                                         heaps.CbvSrvUavShadowCpuHandle(srvSlot_));
        // R13/T1 双写：先落 shadow（该持久槽是 UpdateDescriptorSets/区段迁移
        // 的拷贝源——直写 visible 会让后续读源踩 #654），再镜像进 visible。
        heaps.CopyCbvSrvUavShadowToVisible(srvSlot_);
    }

    // ---- UAV（UnorderedAccess usage；M1 无 view 变体） ----
    if (!viewMode_
        && (texDesc_.usage & TextureUsage::UnorderedAccess) != TextureUsage::Unknown) {
        // 持久槽位（同 SRV——长寿命拷贝源）。
        uavSlot_ = heaps.AllocCbvSrvUavPersistent();
        if (uavSlot_ == D3D12DescriptorHeapManager::kInvalidSlot) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = static_cast<DXGI_FORMAT>(dxgiFull);
        if (texDesc_.type == TextureType::Texture3D) {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uavDesc.Texture3D.MipSlice = 0;
            uavDesc.Texture3D.FirstWSlice = 0;
            uavDesc.Texture3D.WSize = texDesc_.size.z;
        } else if (texDesc_.arraySize > 1) {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.MipSlice = 0;
            uavDesc.Texture2DArray.FirstArraySlice = 0;
            uavDesc.Texture2DArray.ArraySize = texDesc_.arraySize;
            uavDesc.Texture2DArray.PlaneSlice = 0;
        } else {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = 0;
            uavDesc.Texture2D.PlaneSlice = 0;
        }
        native->CreateUnorderedAccessView(resource_.Get(), nullptr, &uavDesc,
                                          heaps.CbvSrvUavShadowCpuHandle(uavSlot_));
        // R13/T1 双写（同 SRV：shadow 落点 + 镜像，拷贝源语义见 SRV 注释）。
        heaps.CopyCbvSrvUavShadowToVisible(uavSlot_);
    }
    return true;
}

void D3D12Texture::destroyImpl() {
    if (!resource_ && !allocation_ && srvSlot_ == kInvalidDescriptorSlot
        && uavSlot_ == kInvalidDescriptorSlot && rtvSlot_ == kInvalidDescriptorSlot
        && dsvSlot_ == kInvalidDescriptorSlot) {
        return;
    }
    D3D12Device& dev = static_cast<D3D12Device&>(device_);

    // 描述符槽位与 D3D12MA 分配均走 GC 延迟（in-flight 提交引用护栏，
    // 对齐 VulkanBuffer destroyImpl 的 DeferredDestroy 模式）。
    D3D12DescriptorHeapManager* heaps = &dev.GetDescriptorHeaps();
    const u32 srv = srvSlot_, uav = uavSlot_, rtv = rtvSlot_, dsv = dsvSlot_;
    D3D12MA::Allocation* allocation = allocation_;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource = std::move(resource_);

    srvSlot_ = uavSlot_ = rtvSlot_ = dsvSlot_ = kInvalidDescriptorSlot;
    allocation_ = nullptr;

    device_.GetGarbageCollector().DeferredDestroy([heaps, srv, uav, rtv, dsv,
                                                   allocation, resource]() {
        if (heaps) {
            // SRV/UAV 持久槽位（拷贝源）显式回收；RTV/DSV 同理。
            // set 实例区段（分帧线性区）随帧槽轮转整体回收——不逐槽 free。
            if (srv != D3D12Texture::kInvalidDescriptorSlot) {
                heaps->FreeCbvSrvUavPersistent(srv);
            }
            if (uav != D3D12Texture::kInvalidDescriptorSlot) {
                heaps->FreeCbvSrvUavPersistent(uav);
            }
            if (rtv != D3D12Texture::kInvalidDescriptorSlot) heaps->FreeRtv(rtv);
            if (dsv != D3D12Texture::kInvalidDescriptorSlot) heaps->FreeDsv(dsv);
        }
        if (allocation) allocation->Release();  // 资源引用随 ComPtr 析构释放
    });
}

// ============================================================================
// 映射 / 上传
// ============================================================================

void* D3D12Texture::mapImpl(u64 /*offset*/, u64 /*size*/) {
    // M1 纹理恒 DEFAULT heap——不支持 CPU 映射（上传走 UpdateData/updateRegion
    // 的 staging 环路径，对齐 VulkanTexture DEVICE_LOCAL 的 map 拒绝语义）。
    PRIMAL_LOG_STREAM(Warn, "RHI")
        << "[D3D12Texture] Map unsupported (DEFAULT heap) — use UpdateData" << std::endl;
    return nullptr;
}

void D3D12Texture::unmapImpl() {}

bool D3D12Texture::updateDataImpl(const void* data, u64 size, u64 offset) {
    // 语义对齐 VulkanTexture::updateDataImpl：mip0/slice0 紧密行主序 blob。
    if (!data || size == 0 || !resource_) return false;

    const u64 bytesPerTexel = DataFormatBytesPerTexel(texDesc_.format);
    const u64 bytesPerRow = u64(texDesc_.size.x) * bytesPerTexel;
    if (bytesPerRow == 0) return false;
    if (offset % bytesPerRow != 0) return false;   // 行粒度子矩形契约
    if (size % bytesPerRow != 0) return false;

    TextureRegionUpdate region{};
    region.mipLevel = 0;
    region.baseArrayLayer = 0;
    region.offsetX = 0;
    region.offsetY = static_cast<u32>(offset / bytesPerRow);
    region.offsetZ = 0;
    region.width = texDesc_.size.x;
    region.height = static_cast<u32>(size / bytesPerRow);
    region.depth = 1;
    if (region.offsetY + region.height > texDesc_.size.y) return false;
    return updateRegion(data, size, region);
}

bool D3D12Texture::updateRegion(const void* data, u64 size, const TextureRegionUpdate& region) {
    if (!data || size == 0 || !resource_) return false;
    D3D12Device& dev = static_cast<D3D12Device&>(device_);

    const u64 bytesPerTexel = DataFormatBytesPerTexel(texDesc_.format);
    if (bytesPerTexel == 0) return false;  // 压缩格式走命令层 CopyBufferToTexture
    // M6 F6-3：3D 上传字节预算含 depth（此前按 2D 平面核算——3D 区域上传
    // 尺寸校验恒误拒；行距取紧密 width*bytesPerTexel，与 updateDataImpl 的
    // 紧密行主序约定一致）。
    const u64 expected = u64(region.width) * bytesPerTexel * region.height
                       * (region.depth ? region.depth : 1);
    if (size != expected) return false;
    if (planeCount_ != 1) {
        // 深度模板双平面格式的 CPU 上传 M1 不支持（NOT-IMPLEMENTED 登记；
        // 主路径深度为 D32_Float 单平面）。
        PRIMAL_LOG_STREAM(Warn, "RHI")
            << "[D3D12Texture] planar format upload NOT-IMPLEMENTED in M1" << std::endl;
        return false;
    }

    return dev.UploadTextureDataViaRing(resource_.Get(), GetHandle(), texDesc_.format,
                                        texDesc_.mipLevels, data, size, region);
}

u64 D3D12Texture::BytesPerTexel() const {
    return DataFormatBytesPerTexel(texDesc_.format);
}

} // namespace primal::graphics::rhi::d3d12

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
