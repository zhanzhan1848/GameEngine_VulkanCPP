/**
 * @file MetalTexture.cpp
 * @brief MetalTexture 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 0.1.0
 */

#include "MetalTexture.h"
#include "MetalDevice.h"
#include <algorithm>
#include <iostream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rhi {

// 辅助函数：获取像素大小（字节）
static u32 GetBytePerPixel(DataFormat format) {
    switch (format) {
        // 8-bit formats
        case DataFormat::R8_UNorm:
        case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:
        case DataFormat::R8_SInt:
            return 1;
            
        // 16-bit formats
        case DataFormat::R16_UNorm:
        case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt:
        case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
        case DataFormat::RG8_UNorm:
        case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt:
        case DataFormat::RG8_SInt:
        case DataFormat::D16_UNorm:
            return 2;
            
        // 32-bit formats
        case DataFormat::R32_UNorm:
        case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt:
        case DataFormat::R32_SInt:
        case DataFormat::R32_Float:
        case DataFormat::RG16_UNorm:
        case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt:
        case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
        case DataFormat::RG8B8A8_UNorm:
        case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt:
        case DataFormat::RG8B8A8_SInt:
        case DataFormat::BGRA8_UNorm:
        case DataFormat::BGRA8_SNorm:
        case DataFormat::BGRA8_UInt:
        case DataFormat::BGRA8_SInt:
        case DataFormat::RGBA8_UNorm:
        case DataFormat::RGBA8_SNorm:
        case DataFormat::RGBA8_UInt:
        case DataFormat::RGBA8_SInt:
        case DataFormat::RGBA8_sRGB:
        case DataFormat::D32_Float:
        case DataFormat::D24_UNorm_S8_UInt: // Packed 32-bit
            return 4;
            
        // 64-bit formats
        case DataFormat::RG32_UNorm:
        case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt:
        case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
        case DataFormat::RGBA16_UNorm:
        case DataFormat::RGBA16_SNorm:
        case DataFormat::RGBA16_UInt:
        case DataFormat::RGBA16_SInt:
        case DataFormat::RGBA16_Float:
            return 8;
            
        // 96-bit formats (RGB32) - Metal usually treats these as RGBA32
        case DataFormat::RGB32_UNorm:
        case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt:
        case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
            return 12; // Or 16 if aligned
            
        // 128-bit formats
        case DataFormat::RGBA32_UNorm:
        case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt:
        case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
        case DataFormat::RGBA32_sRGB:
            return 16;
            
        // Compressed formats (Block size usually 16 bytes for 4x4, but returning 0 for now as they need special handling)
        // BC1/BC4: 8 bytes per 4x4 block
        // BC2/BC3/BC5/BC6/BC7: 16 bytes per 4x4 block
        
        default:
            return 0;
    }
}

// 辅助函数：将 DataFormat 转换为 MTLPixelFormat
static MTL::PixelFormat ToMTLPixelFormat(DataFormat format) {
    switch (format) {
        case DataFormat::R8_UNorm: return MTL::PixelFormatR8Unorm;
        case DataFormat::R8_SNorm: return MTL::PixelFormatR8Snorm;
        case DataFormat::R8_UInt: return MTL::PixelFormatR8Uint;
        case DataFormat::R8_SInt: return MTL::PixelFormatR8Sint;
        
        case DataFormat::R16_UNorm: return MTL::PixelFormatR16Unorm;
        case DataFormat::R16_SNorm: return MTL::PixelFormatR16Snorm;
        case DataFormat::R16_UInt: return MTL::PixelFormatR16Uint;
        case DataFormat::R16_SInt: return MTL::PixelFormatR16Sint;
        case DataFormat::R16_Float: return MTL::PixelFormatR16Float;
        
        case DataFormat::RG8_UNorm: return MTL::PixelFormatRG8Unorm;
        case DataFormat::RG8_SNorm: return MTL::PixelFormatRG8Snorm;
        case DataFormat::RG8_UInt: return MTL::PixelFormatRG8Uint;
        case DataFormat::RG8_SInt: return MTL::PixelFormatRG8Sint;
        
        // Metal doesn't support RGB8 (24-bit). Use RGBA8.
        case DataFormat::R8G8B8_UNorm: return MTL::PixelFormatInvalid;
        case DataFormat::R8G8B8_SNorm: return MTL::PixelFormatInvalid;
        case DataFormat::R8G8B8_UInt: return MTL::PixelFormatInvalid;
        case DataFormat::R8G8B8_SInt: return MTL::PixelFormatInvalid;
        
        case DataFormat::R32_UNorm: return MTL::PixelFormatR32Float; // R32Unorm not supported, fallback to Float or Invalid
        case DataFormat::R32_SNorm: return MTL::PixelFormatR32Float; 
        case DataFormat::R32_UInt: return MTL::PixelFormatR32Uint;
        case DataFormat::R32_SInt: return MTL::PixelFormatR32Sint;
        case DataFormat::R32_Float: return MTL::PixelFormatR32Float;
        
        case DataFormat::RG16_UNorm: return MTL::PixelFormatRG16Unorm;
        case DataFormat::RG16_SNorm: return MTL::PixelFormatRG16Snorm;
        case DataFormat::RG16_UInt: return MTL::PixelFormatRG16Uint;
        case DataFormat::RG16_SInt: return MTL::PixelFormatRG16Sint;
        case DataFormat::RG16_Float: return MTL::PixelFormatRG16Float;
        
        case DataFormat::RG8B8A8_UNorm: return MTL::PixelFormatRGBA8Unorm;
        case DataFormat::RG8B8A8_SNorm: return MTL::PixelFormatRGBA8Snorm;
        case DataFormat::RG8B8A8_UInt: return MTL::PixelFormatRGBA8Uint;
        case DataFormat::RG8B8A8_SInt: return MTL::PixelFormatRGBA8Sint;
        
        case DataFormat::BGRA8_UNorm: return MTL::PixelFormatBGRA8Unorm;
        case DataFormat::BGRA8_SNorm: return MTL::PixelFormatInvalid; // Not standard
        case DataFormat::BGRA8_UInt: return MTL::PixelFormatInvalid;
        case DataFormat::BGRA8_SInt: return MTL::PixelFormatInvalid;
        
        case DataFormat::RGBA8_UNorm: return MTL::PixelFormatRGBA8Unorm;
        case DataFormat::RGBA8_SNorm: return MTL::PixelFormatRGBA8Snorm;
        case DataFormat::RGBA8_UInt: return MTL::PixelFormatRGBA8Uint;
        case DataFormat::RGBA8_SInt: return MTL::PixelFormatRGBA8Sint;
        case DataFormat::RGBA8_sRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;

        
        case DataFormat::RG32_UNorm: return MTL::PixelFormatRG32Float; // Fallback
        case DataFormat::RG32_SNorm: return MTL::PixelFormatRG32Float;
        case DataFormat::RG32_UInt: return MTL::PixelFormatRG32Uint;
        case DataFormat::RG32_SInt: return MTL::PixelFormatRG32Sint;
        case DataFormat::RG32_Float: return MTL::PixelFormatRG32Float;
        
        case DataFormat::RGBA16_UNorm: return MTL::PixelFormatRGBA16Unorm;
        case DataFormat::RGBA16_SNorm: return MTL::PixelFormatRGBA16Snorm;
        case DataFormat::RGBA16_UInt: return MTL::PixelFormatRGBA16Uint;
        case DataFormat::RGBA16_SInt: return MTL::PixelFormatRGBA16Sint;
        case DataFormat::RGBA16_Float: return MTL::PixelFormatRGBA16Float;
        
        case DataFormat::RGBA32_UNorm: return MTL::PixelFormatRGBA32Float;
        case DataFormat::RGBA32_SNorm: return MTL::PixelFormatRGBA32Float;
        case DataFormat::RGBA32_UInt: return MTL::PixelFormatRGBA32Uint;
        case DataFormat::RGBA32_SInt: return MTL::PixelFormatRGBA32Sint;
        case DataFormat::RGBA32_Float: return MTL::PixelFormatRGBA32Float;
        
        // Depth/Stencil
        case DataFormat::D16_UNorm: return MTL::PixelFormatDepth16Unorm;
        case DataFormat::D32_Float: return MTL::PixelFormatDepth32Float;
        case DataFormat::D24_UNorm_S8_UInt: return MTL::PixelFormatDepth24Unorm_Stencil8;
        case DataFormat::D32_Float_S8X24_UInt: return MTL::PixelFormatDepth32Float_Stencil8;

        // VT M0.8: ASTC(Apple GPU 原生;离散 Mac 不支持 — 见 MetalDevice::queryDeviceInfo)
        case DataFormat::ASTC_4x4_UNorm: return MTL::PixelFormatASTC_4x4_LDR;
        case DataFormat::ASTC_4x4_sRGB: return MTL::PixelFormatASTC_4x4_sRGB;
        case DataFormat::ASTC_6x6_UNorm: return MTL::PixelFormatASTC_6x6_LDR;
        case DataFormat::ASTC_6x6_sRGB: return MTL::PixelFormatASTC_6x6_sRGB;
        case DataFormat::ASTC_8x8_UNorm: return MTL::PixelFormatASTC_8x8_LDR;
        case DataFormat::ASTC_8x8_sRGB: return MTL::PixelFormatASTC_8x8_sRGB;

        default: return MTL::PixelFormatInvalid;
    }
}

// 辅助函数：将 TextureType 转换为 MTLTextureType
static MTL::TextureType ToMTLTextureType(TextureType type) {
    switch (type) {
        case TextureType::Texture1D: return MTL::TextureType1D;
        case TextureType::Texture2D: return MTL::TextureType2D;
        case TextureType::Texture3D: return MTL::TextureType3D;
        case TextureType::TextureCube: return MTL::TextureTypeCube;
        case TextureType::Texture1DArray: return MTL::TextureType1DArray;
        case TextureType::Texture2DArray: return MTL::TextureType2DArray;
        case TextureType::TextureCubeArray: return MTL::TextureTypeCubeArray;
        default: return MTL::TextureType2D;
    }
}

// 辅助函数：将 TextureUsage 转换为 ResourceUsage
static ResourceUsage GetResourceUsageFromTextureUsage(TextureUsage usage) {
    return static_cast<ResourceUsage>(usage);
}

MetalTexture::MetalTexture(MetalDevice& device, const TextureDesc& desc)
    : RHIResource(device, ResourceDesc(
        ResourceType::Texture,
        GetResourceUsageFromTextureUsage(desc.usage),
        desc.memoryUsage,
        0, // Size is not easily known here without calculation, but RHIResource desc_ stores it.
        desc.name.c_str()
      )),
      textureDesc_(desc)
{
    // Calculate estimated size
    // desc_.size = ...;
}

MetalTexture::MetalTexture(MetalTexture&& other) noexcept
    : RHIResource(std::move(other)), textureDesc_(other.textureDesc_), mtlTexture_(other.mtlTexture_) {
    other.mtlTexture_ = nullptr;
}

MetalTexture& MetalTexture::operator=(MetalTexture&& other) noexcept {
    if (this != &other) {
        destroyImpl();
        RHIResource::operator=(std::move(other));
        textureDesc_ = other.textureDesc_;
        mtlTexture_ = other.mtlTexture_;
        other.mtlTexture_ = nullptr;
    }
    return *this;
}

MetalTexture::~MetalTexture() {
    destroyImpl();
}

bool MetalTexture::Initialize() {
    // Check if this is a swapchain texture
    if (desc_.memoryUsage == GPUMemoryUsage::SwapChain) {
        // SwapChain textures are managed by the system/swapchain, 
        // we don't create an MTL::Texture here.
        // It will be set later via SetNativeTexture.
        state_ = ResourceState::Allocated;
        return true;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    
    descriptor->setTextureType(ToMTLTextureType(textureDesc_.type));
    descriptor->setPixelFormat(ToMTLPixelFormat(textureDesc_.format));
    descriptor->setWidth(textureDesc_.size.x);
    descriptor->setHeight(textureDesc_.size.y);
    descriptor->setDepth(textureDesc_.size.z);
    descriptor->setMipmapLevelCount(textureDesc_.mipLevels);
    descriptor->setArrayLength(textureDesc_.arraySize);
    
    // Usage
    MTL::TextureUsage usage = MTL::TextureUsageUnknown;
    if (HasUsage(desc_.usage, ResourceUsage::ShaderResource)) usage |= MTL::TextureUsageShaderRead;
    if (HasUsage(desc_.usage, ResourceUsage::RenderTarget)) usage |= MTL::TextureUsageRenderTarget;
    if (HasUsage(desc_.usage, ResourceUsage::UnorderedAccess)) usage |= MTL::TextureUsageShaderWrite; // Or ShaderReadWrite
    if (HasUsage(desc_.usage, ResourceUsage::DepthStencil)) usage |= MTL::TextureUsageRenderTarget; // Metal uses RenderTarget for depth too
    descriptor->setUsage(usage);
    if (textureDesc_.format == DataFormat::RG32_Float && textureDesc_.size.x == 2048) {
        static int usageDbg = 0;
        if (usageDbg < 8) { ++usageDbg;
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MTLUSAGE] RG32F 2048 desc.usage=" << (int)textureDesc_.usage
                      << " mtl.usage=" << (int)usage << std::endl;
        }
    }
    
    // Storage Mode
    MTL::StorageMode storageMode = MTL::StorageModePrivate;
    if (desc_.memoryUsage == GPUMemoryUsage::Staging || desc_.memoryUsage == GPUMemoryUsage::Readback) {
        #if TARGET_OS_OSX
        storageMode = MTL::StorageModeManaged;
        #else
        storageMode = MTL::StorageModeShared;
        #endif
    } else {
        storageMode = MTL::StorageModePrivate;
    }
    descriptor->setStorageMode(storageMode);

    // Hazard tracking — Metal only auto-tracks render-target textures.
    // Compute-written textures (StorageImage/UAV) default to Untracked,
    // meaning Metal does NOT serialize cross-cmdbuf read/write hazards.
    // That breaks producer/consumer patterns like SurfaceNets (compute write
    // in one cmdbuf) → DrawIndirect (read in next cmdbuf): the read can
    // overlap with the write, returning stale or torn data. Force Tracked
    // for any UAV texture so Metal inserts the necessary fences.
    if (HasUsage(desc_.usage, ResourceUsage::UnorderedAccess)) {
        descriptor->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
        static int s_tracked_count = 0;
        if (s_tracked_count < 30) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] UAV texture tracked: " << desc_.name
                      << " size=" << textureDesc_.size.x << "x" << textureDesc_.size.y << "x" << textureDesc_.size.z
                      << " fmt=" << static_cast<int>(textureDesc_.format) << std::endl;
            ++s_tracked_count;
        }
    }
    
    // Create Texture
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    mtlTexture_ = metalDevice.GetNativeDevice()->newTexture(descriptor);
    
    descriptor->release();
    
    if (!mtlTexture_) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] Failed to create texture: " << desc_.name << std::endl;
        return false;
    }
    
    // Update size in base class
    // desc_.size = ...; // Not accessible? desc_ is protected.
    // Yes, desc_ is protected in RHIResource.
    // mtlTexture_->allocatedSize() might give 0 for private textures on some devices until allocated?
    // But we can try.
    // desc_.size = mtlTexture_->allocatedSize(); 
    
    state_ = ResourceState::Allocated;
    return true;
}

void MetalTexture::destroyImpl() {
    auto mtlTexture = mtlTexture_;
    if (mtlTexture) {
        device_.GetGarbageCollector().DeferredDestroy([mtlTexture]() {
            mtlTexture->release();
        });
        mtlTexture_ = nullptr;
    }
    state_ = ResourceState::Destroyed;
}

void* MetalTexture::mapImpl(u64 offset, u64 size) {
    // Textures are generally not mappable in the same way as buffers
    // Only linear textures or managed/shared textures might be mappable via getBytes/replaceRegion logic or if backed by buffer
    // For now, return nullptr
    return nullptr;
}

void MetalTexture::unmapImpl() {
    // Do nothing
}

bool MetalTexture::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!mtlTexture_ || !data) return false;
    
    // Simplified update for 2D texture, single mip, single layer
    // For full support, we need to know row pitch, slice pitch, etc.
    // This interface updateDataImpl(void*, size, offset) is very buffer-centric.
    // RHIResource might need a better interface for texture update, or we assume this is a raw blob update?
    
    // If we assume the data is tightly packed for the whole texture (level 0)
    // We can try to upload.
    
    // Calculate bytes per row
    // This is tricky without knowing the format block size.
    // For now, assume RGBA8 (4 bytes per pixel) as a fallback or strict requirement for this simple impl?
    // Or we can assume the user provided `size` matches the texture size and calculate rowBytes from width.
    
    // This is a limitation of the current RHIResource interface if it treats everything as a linear blob.
    // We will implement basic support for common formats.
    
    // Estimate bytes per pixel
    u32 bytesPerPixel = GetBytePerPixel(textureDesc_.format);
    if (bytesPerPixel == 0) {
        // Fallback or compressed format handling needed
        // For now, assume 4 bytes as a safe fallback for unknown formats in basic tests
        bytesPerPixel = 4;
    }
    
    NS::UInteger bytesPerRow = textureDesc_.size.x * bytesPerPixel;
    NS::UInteger bytesPerImage = bytesPerRow * textureDesc_.size.y;
    
    // Check storage mode
    if (mtlTexture_->storageMode() == MTL::StorageModePrivate) {
        // 异步路径: per-frame staging pool + deferred blit (encoded by MetalDevice::submitImpl)
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalStagingAllocator& allocator = metalDevice.GetStagingAllocator();

        if (!allocator.IsInitialized()) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] Staging allocator not initialized" << std::endl;
            return false;
        }

        MetalStagingAllocator::Allocation alloc = allocator.Allocate(size, 256);
        if (alloc.overflow) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] Staging pool overflow, size=" << size << std::endl;
            return false;
        }

        std::memcpy(alloc.cpuPtr, data, size);

        MTL::Size sourceSize = MTL::Size::Make(textureDesc_.size.x, textureDesc_.size.y, 1);
        allocator.QueueBlit_Texture(alloc, mtlTexture_,
                                    /*mipLevel=*/0, /*slice=*/0,
                                    /*origin=*/0, 0, 0,
                                    /* dimensions matching sourceSize */
                                    textureDesc_.size.x, textureDesc_.size.y, 1,
                                    bytesPerRow, bytesPerImage);
    } else {
        // Shared or Managed: use replaceRegion
        MTL::Region region = MTL::Region::Make2D(0, 0, textureDesc_.size.x, textureDesc_.size.y);
        mtlTexture_->replaceRegion(region, 0, data, bytesPerRow);
    }

    return true;
}

// VT M0.7: 任意子区域上传(镜像 VulkanTexture::updateRegion 语义)
bool MetalTexture::updateRegion(const void* data, u64 size, const TextureRegionUpdate& region) {
    if (!mtlTexture_ || !data || size == 0) return false;
    if (textureDesc_.type != TextureType::Texture2D || textureDesc_.size.z > 1 ||
        textureDesc_.arraySize > 1) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: only 2D single-slice supported (M0 scope)" << std::endl;
        return false;
    }
    // 块压缩格式(BC/ASTC)不支持紧密行主序线性布局
    switch (textureDesc_.format) {
        case DataFormat::BC1_UNorm: case DataFormat::BC1_sRGB:
        case DataFormat::BC2_UNorm: case DataFormat::BC2_sRGB:
        case DataFormat::BC3_UNorm: case DataFormat::BC3_sRGB:
        case DataFormat::BC4_UNorm: case DataFormat::BC4_SNorm:
        case DataFormat::BC5_UNorm: case DataFormat::BC5_SNorm:
        case DataFormat::BC6H_UF16: case DataFormat::BC6H_SF16:
        case DataFormat::BC7_UNorm: case DataFormat::BC7_sRGB:
        case DataFormat::ASTC_4x4_UNorm: case DataFormat::ASTC_4x4_sRGB:
        case DataFormat::ASTC_6x6_UNorm: case DataFormat::ASTC_6x6_sRGB:
        case DataFormat::ASTC_8x8_UNorm: case DataFormat::ASTC_8x8_sRGB:
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: block-compressed formats unsupported — "
                         "use CopyBufferToTexture" << std::endl;
            return false;
        default: break;
    }
    const u32 bytesPerPixel = GetBytePerPixel(textureDesc_.format);
    if (bytesPerPixel == 0) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: unknown texel size for format "
                  << static_cast<int>(textureDesc_.format) << std::endl;
        return false;
    }
    if (region.mipLevel >= textureDesc_.mipLevels) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: mip " << region.mipLevel
                  << " >= mipLevels " << textureDesc_.mipLevels << std::endl;
        return false;
    }
    if (region.offsetZ != 0 || region.depth != 1) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: 2D requires offsetZ=0/depth=1" << std::endl;
        return false;
    }
    const u32 mipW = std::max<u32>(1u, textureDesc_.size.x >> region.mipLevel);
    const u32 mipH = std::max<u32>(1u, textureDesc_.size.y >> region.mipLevel);
    if (region.width == 0 || region.height == 0 ||
        region.offsetX + region.width > mipW || region.offsetY + region.height > mipH) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: region [" << region.offsetX << ","
                  << region.offsetY << " " << region.width << "x" << region.height
                  << ") exceeds mip " << region.mipLevel << " dims "
                  << mipW << "x" << mipH << std::endl;
        return false;
    }
    const NS::UInteger bytesPerRow = NS::UInteger(region.width) * bytesPerPixel;
    const NS::UInteger bytesPerImage = bytesPerRow * region.height;
    if (size != u64(bytesPerImage)) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: size " << size << " != tightly-packed "
                  << bytesPerImage << std::endl;
        return false;
    }

    if (mtlTexture_->storageMode() == MTL::StorageModePrivate) {
        // 异步路径:staging 队列(下一 submit 的 EncodePendingBlits 编码)
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MetalStagingAllocator& allocator = metalDevice.GetStagingAllocator();
        if (!allocator.IsInitialized()) {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: staging allocator not initialized" << std::endl;
            return false;
        }
        MetalStagingAllocator::Allocation alloc = allocator.Allocate(size, 256);
        if (alloc.overflow) {
            // 排干队列后重试一次(对齐 Vulkan 侧 overflow 约定)
            allocator.FlushBlocking();
            alloc = allocator.Allocate(size, 256);
            if (alloc.overflow) {
                PRIMAL_LOG_STREAM(Error, "RHI") << "[MetalTexture] updateRegion: staging pool overflow, size="
                          << size << std::endl;
                return false;
            }
        }
        std::memcpy(alloc.cpuPtr, data, size);
        allocator.QueueBlit_Texture(alloc, mtlTexture_,
                                    region.mipLevel, region.baseArrayLayer,
                                    region.offsetX, region.offsetY, region.offsetZ,
                                    region.width, region.height, region.depth,
                                    u32(bytesPerRow), u32(bytesPerImage));
    } else {
        // Shared/Managed: replaceRegion 立即生效
        MTL::Region mtlRegion = MTL::Region::Make2D(region.offsetX, region.offsetY,
                                                    region.width, region.height);
        mtlTexture_->replaceRegion(mtlRegion, region.mipLevel, data, bytesPerRow);
    }
    return true;
}

} // namespace primal::graphics::rhi
