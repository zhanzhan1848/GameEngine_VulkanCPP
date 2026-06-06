#include "DawnTexture.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include <iostream>

namespace primal::graphics::rhi {

// ============================================================
// Construction / Destruction
// ============================================================

DawnTexture::DawnTexture(DawnDevice& device, const TextureDesc& desc)
    : RHIResource(device, ResourceDesc(ResourceType::Texture,
                                       ResourceUsage::None,
                                       desc.memoryUsage,
                                       0, // size computed below
                                       desc.name.empty() ? nullptr : desc.name.c_str())),
      device_(device),
      textureDesc_(desc) {}

DawnTexture::~DawnTexture() {
    if (wgpuTexture_ || wgpuTextureView_) {
        destroyImpl();
    }
}

// ============================================================
// Initialize – create WGPUTexture + default view
// ============================================================

bool DawnTexture::Initialize() {
    if (wgpuTexture_) {
        return true; // Already initialized
    }

    // --- Convert TextureDesc to WGPUTextureDescriptor ---

    WGPUTextureDescriptor texDesc{};
    texDesc.nextInChain = nullptr;
    texDesc.label = textureDesc_.name.empty() ? ToWGPUStringView("") : ToWGPUStringView(textureDesc_.name.c_str());

    // Texture dimension from TextureType
    texDesc.dimension = ToWGPUTextureDimension(textureDesc_.type);

    // Size
    texDesc.size.width = textureDesc_.size.x;
    texDesc.size.height = textureDesc_.size.y;
    texDesc.size.depthOrArrayLayers = textureDesc_.size.z;

    // For 2D textures with array, depthOrArrayLayers = arraySize
    if (textureDesc_.type == TextureType::Texture2DArray ||
        textureDesc_.type == TextureType::TextureCubeArray) {
        texDesc.size.depthOrArrayLayers = textureDesc_.arraySize;
    }
    // For cube textures, array layers = 6 * arraySize
    if (textureDesc_.type == TextureType::TextureCube) {
        texDesc.size.depthOrArrayLayers = 6;
    } else if (textureDesc_.type == TextureType::TextureCubeArray) {
        texDesc.size.depthOrArrayLayers = 6 * textureDesc_.arraySize;
    }

    // For 1D textures, height and depth must be 1
    if (textureDesc_.type == TextureType::Texture1D ||
        textureDesc_.type == TextureType::Texture1DArray) {
        texDesc.size.height = 1;
        texDesc.size.depthOrArrayLayers = 1;
        if (textureDesc_.type == TextureType::Texture1DArray) {
            texDesc.size.depthOrArrayLayers = textureDesc_.arraySize;
        }
    }

    // Format
    texDesc.format = ToWGPUTextureFormat(textureDesc_.format);
    if (texDesc.format == WGPUTextureFormat_Undefined) {
        std::cerr << "[DawnTexture] Unsupported texture format: "
                  << static_cast<int>(textureDesc_.format) << std::endl;
        return false;
    }

    // Mip levels
    texDesc.mipLevelCount = textureDesc_.mipLevels;

    // Sample count
    texDesc.sampleCount = 1; // Default; multisample textures set this via desc

    // --- Compute WGPUTextureUsage flags from TextureUsage ---

    WGPUTextureUsage usage = WGPUTextureUsage_None;

    // Check individual usage bits from TextureUsage flags
    if ((textureDesc_.usage & TextureUsage::RenderTarget) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_RenderAttachment;
    }
    if ((textureDesc_.usage & TextureUsage::DepthStencil) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_RenderAttachment;
    }
    if ((textureDesc_.usage & TextureUsage::ShaderResource) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_TextureBinding;
    }
    if ((textureDesc_.usage & TextureUsage::UnorderedAccess) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_StorageBinding;
    }
    if ((textureDesc_.usage & TextureUsage::CopySource) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_CopySrc;
    }
    if ((textureDesc_.usage & TextureUsage::CopyDest) != TextureUsage::Unknown) {
        usage |= WGPUTextureUsage_CopyDst;
    }

    // Always add CopyDst for initial data upload support
    usage |= WGPUTextureUsage_CopyDst;

    texDesc.usage = usage;

    // --- Create the texture ---

    wgpuTexture_ = wgpuDeviceCreateTexture(device_.GetNativeDevice(), &texDesc);
    if (!wgpuTexture_) {
        std::cerr << "[DawnTexture] Failed to create texture"
                  << (textureDesc_.name.empty() ? "" : (" '" + textureDesc_.name + "'").c_str())
                  << std::endl;
        return false;
    }

    // --- Create the default texture view ---

    // Passing nullptr creates a view covering all mips and all array layers
    // with the same format as the texture.
    wgpuTextureView_ = wgpuTextureCreateView(wgpuTexture_, nullptr);
    if (!wgpuTextureView_) {
        std::cerr << "[DawnTexture] Failed to create default view for texture"
                  << (textureDesc_.name.empty() ? "" : (" '" + textureDesc_.name + "'").c_str())
                  << std::endl;
        // Texture itself was created successfully; view failure is non-fatal.
        // The caller can retry with CreateView().
    }

    SetState(ResourceState::Ready);
    return true;
}

// ============================================================
// CreateView – create a custom WGPUTextureView
// ============================================================

WGPUTextureView DawnTexture::CreateView(const TextureViewDesc& viewDesc) {
    if (!wgpuTexture_) {
        std::cerr << "[DawnTexture] Cannot create view: texture not initialized" << std::endl;
        return nullptr;
    }

    WGPUTextureViewDescriptor wgpuViewDesc{};
    wgpuViewDesc.nextInChain = nullptr;
    wgpuViewDesc.label = ToWGPUStringView("CustomView");

    // Format
    wgpuViewDesc.format = ToWGPUTextureFormat(viewDesc.format);
    if (wgpuViewDesc.format == WGPUTextureFormat_Undefined) {
        // Fall back to the texture's own format
        wgpuViewDesc.format = ToWGPUTextureFormat(textureDesc_.format);
    }

    // Dimension from view type
    switch (viewDesc.viewType) {
        case TextureType::Texture1D:
        case TextureType::Texture1DArray:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_1D;
            break;
        case TextureType::Texture2D:
        case TextureType::Texture2DArray:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_2D;
            if (viewDesc.viewType == TextureType::Texture2DArray) {
                wgpuViewDesc.dimension = WGPUTextureViewDimension_2DArray;
            }
            break;
        case TextureType::Texture3D:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_3D;
            break;
        case TextureType::TextureCube:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_Cube;
            break;
        case TextureType::TextureCubeArray:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_CubeArray;
            break;
        default:
            wgpuViewDesc.dimension = WGPUTextureViewDimension_2D;
            break;
    }

    // Mip range
    wgpuViewDesc.baseMipLevel = viewDesc.mostDetailedMip;
    wgpuViewDesc.mipLevelCount = (viewDesc.mipCount == 0) ? WGPU_MIP_LEVEL_COUNT_UNDEFINED : viewDesc.mipCount;

    // Array range
    wgpuViewDesc.baseArrayLayer = viewDesc.firstArraySlice;
    wgpuViewDesc.arrayLayerCount = viewDesc.arraySize;

    // Aspect
    wgpuViewDesc.aspect = WGPUTextureAspect_All;

    // Determine aspect from format (depth/stencil textures)
    DataFormat fmt = viewDesc.format;
    if (fmt == DataFormat::Unknown) {
        fmt = textureDesc_.format;
    }
    if (fmt == DataFormat::D16_UNorm || fmt == DataFormat::D32_Float) {
        wgpuViewDesc.aspect = WGPUTextureAspect_DepthOnly;
    } else if (fmt == DataFormat::D24_UNorm_S8_UInt ||
               fmt == DataFormat::D32_Float_S8X24_UInt) {
        wgpuViewDesc.aspect = WGPUTextureAspect_DepthOnly;
        // Note: For stencil-only view, caller should use a separate
        // CreateView call with aspect override. This is a future enhancement.
    }

    WGPUTextureView view = wgpuTextureCreateView(wgpuTexture_, &wgpuViewDesc);
    if (!view) {
        std::cerr << "[DawnTexture] Failed to create custom texture view" << std::endl;
    }

    return view;
}

// ============================================================
// destroyImpl – release view and texture
// ============================================================

void DawnTexture::destroyImpl() {
    if (wgpuTextureView_) {
        wgpuTextureViewRelease(wgpuTextureView_);
        wgpuTextureView_ = nullptr;
    }

    if (wgpuTexture_) {
        wgpuTextureRelease(wgpuTexture_);
        wgpuTexture_ = nullptr;
    }
}

void DawnTexture::OverrideNativeTexture(WGPUTexture texture, WGPUTextureView view) {
    // Don't release previous handles — caller is responsible for that.
    wgpuTexture_ = texture;
    wgpuTextureView_ = view;
}

void DawnTexture::ClearNativeHandles() {
    // Don't release — the swapchain owns the WGPUTexture.
    wgpuTexture_ = nullptr;
    wgpuTextureView_ = nullptr;
}

void DawnTexture::ReleaseWrappedHandles() {
    // Release only the texture view we created — the texture itself is owned by the surface.
    if (wgpuTextureView_) {
        wgpuTextureViewRelease(wgpuTextureView_);
        wgpuTextureView_ = nullptr;
    }
    wgpuTexture_ = nullptr;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
