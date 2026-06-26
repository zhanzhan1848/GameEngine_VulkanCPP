/**
 * @file MetalTexture.cpp
 * @brief MetalTexture 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 0.1.0
 */

#include "MetalTexture.h"
#include "MetalDevice.h"
#include <iostream>

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
            std::cerr << "[MetalTexture] UAV texture tracked: " << desc_.name
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
        std::cerr << "[MetalTexture] Failed to create texture: " << desc_.name << std::endl;
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
        // Use Staging Buffer
        MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
        MTL::Device* mtlDevice = metalDevice.GetNativeDevice();
        
        // Create staging buffer
        MTL::Buffer* stagingBuffer = mtlDevice->newBuffer(size, MTL::ResourceStorageModeShared);
        if (!stagingBuffer) {
            std::cerr << "[MetalTexture] Failed to create staging buffer. Size: " << size << std::endl;
            return false;
        }
        
        // Copy data to staging buffer
        memcpy(stagingBuffer->contents(), data, size);
        
        // Create command buffer and blit encoder
        MTL::CommandQueue* queue = metalDevice.GetTransferQueue();
        if (!queue) {
            std::cerr << "[MetalTexture] Transfer queue is null" << std::endl;
            stagingBuffer->release();
            return false;
        }

        MTL::CommandBuffer* cmdBuffer = queue->commandBuffer();
        if (!cmdBuffer) {
            std::cerr << "[MetalTexture] Failed to create command buffer" << std::endl;
            stagingBuffer->release();
            return false;
        }

        MTL::BlitCommandEncoder* blitEncoder = cmdBuffer->blitCommandEncoder();
        if (!blitEncoder) {
            std::cerr << "[MetalTexture] Failed to create blit encoder" << std::endl;
            // cmdBuffer->release(); // Autoreleased
            stagingBuffer->release();
            return false;
        }
        
        // Copy from buffer to texture
        MTL::Size sourceSize = MTL::Size::Make(textureDesc_.size.x, textureDesc_.size.y, 1);
        
        // Blit copy
        blitEncoder->copyFromBuffer(
            stagingBuffer,
            0,
            bytesPerRow,
            bytesPerImage,
            sourceSize,
            mtlTexture_,
            0,
            0,
            MTL::Origin::Make(0, 0, 0)
        );
        
        blitEncoder->endEncoding();
        
        // Add completion handler to release staging buffer
        // Note: In C++ Metal-cpp, we need to handle object lifetime carefully.
        // We can release stagingBuffer after command buffer completion.
        // Or simply wait here for synchronous update (easiest for now).
        
        cmdBuffer->commit();
        cmdBuffer->waitUntilCompleted();
        
        stagingBuffer->release();
    } else {
        // Shared or Managed: use replaceRegion
        MTL::Region region = MTL::Region::Make2D(0, 0, textureDesc_.size.x, textureDesc_.size.y);
        mtlTexture_->replaceRegion(region, 0, data, bytesPerRow);
    }
    
    return true;
}

} // namespace primal::graphics::rhi

