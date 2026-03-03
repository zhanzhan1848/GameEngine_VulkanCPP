/**
 * @file RHIResource.cpp
 * @brief RHI资源基类实现
 * @details RHI资源基类的非虚函数实现和工具函数
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#include "RHIResource.h"
#include "RHIDevice.h"
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <cstdio>
#include <memory>

namespace primal::graphics::rhi {

/**
 * @brief 获取数据格式的字节大小
 * @param format 数据格式
 * @return 字节大小
 */
u32 GetFormatSize(DataFormat format) {
    switch (format) {
        case DataFormat::Unknown:
            return 0;
        case DataFormat::R8_UNorm:
        case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:
        case DataFormat::R8_SInt:
            return 1;
        case DataFormat::R16_UNorm:
        case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt:
        case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
            return 2;
        case DataFormat::RG8_UNorm:
        case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt:
        case DataFormat::RG8_SInt:
            return 2;
        case DataFormat::R32_UNorm:
        case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt:
        case DataFormat::R32_SInt:
        case DataFormat::R32_Float:
            return 4;
        case DataFormat::RG16_UNorm:
        case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt:
        case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
            return 4;
        case DataFormat::RG8B8A8_UNorm:
        case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt:
        case DataFormat::RG8B8A8_SInt:
            return 4;
        case DataFormat::RG32_UNorm:
        case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt:
        case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
            return 8;
        case DataFormat::RGB32_UNorm:
        case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt:
        case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
            return 12;
        case DataFormat::RGBA32_UNorm:
        case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt:
        case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
            return 16;
        case DataFormat::R8G8B8_UNorm:
        case DataFormat::R8G8B8_SNorm:
        case DataFormat::R8G8B8_UInt:
        case DataFormat::R8G8B8_SInt:
            return 3;
        case DataFormat::BGRA8_UNorm:
        case DataFormat::BGRA8_SNorm:
        case DataFormat::BGRA8_UInt:
        case DataFormat::BGRA8_SInt:
            return 4;
        case DataFormat::BC1_UNorm:
        case DataFormat::BC1_sRGB:
            return 8;  // 每个块4x4像素，每像素0.5字节
        case DataFormat::BC2_UNorm:
        case DataFormat::BC2_sRGB:
        case DataFormat::BC3_UNorm:
        case DataFormat::BC3_sRGB:
            return 16; // 每个块4x4像素，每像素1字节
        case DataFormat::BC4_UNorm:
        case DataFormat::BC4_SNorm:
            return 8;  // 每个块4x4像素，每像素0.5字节
        case DataFormat::BC5_UNorm:
        case DataFormat::BC5_SNorm:
            return 16; // 每个块4x4像素，每像素1字节
        case DataFormat::BC6H_UF16:
        case DataFormat::BC6H_SF16:
        case DataFormat::BC7_UNorm:
        case DataFormat::BC7_sRGB:
            return 16; // 每个块4x4像素，每像素1字节
        default:
            return 0;
    }
}

RHIResource& RHIResource::operator=(RHIResource&& other) noexcept {
    if (this != &other) {
        Destroy();
        
        device_ = other.device_;
        refCount_ = other.refCount_.exchange(0);
        desc_ = other.desc_;
        handle_ = other.handle_;
        state_ = other.state_;
        refCount_ = other.refCount_.load();
        mappedData_ = other.mappedData_;
        
        other.handle_ = handles::INVALID_RESOURCE;
        other.state_ = ResourceState::Destroyed;
        other.refCount_ = 0;
        other.mappedData_ = nullptr;
    }
    return *this;
}

// === 资源工厂 ===

/**
 * @brief 资源工厂类
 * @details 用于创建不同类型的资源
 */
class ResourceFactory {
public:
    /**
     * @brief 创建缓冲区资源
     * @param device 设备引用
     * @param desc 缓冲区描述符
     * @return 资源指针，失败返回nullptr
     */
    static std::unique_ptr<RHIResource> CreateBuffer(RHIDeviceBase& device, const BufferDesc& desc) {
        // 这里应该根据平台创建对应的缓冲区资源
        // 目前返回nullptr，需要在派生类中实现
        (void)device;
        (void)desc;
        return nullptr;
    }
    
    /**
     * @brief 创建纹理资源
     * @param device 设备引用
     * @param desc 纹理描述符
     * @return 资源指针，失败返回nullptr
     */
    static std::unique_ptr<RHIResource> CreateTexture(RHIDeviceBase& device, const TextureDesc& desc) {
        // 这里应该根据平台创建对应的纹理资源
        // 目前返回nullptr，需要在派生类中实现
        (void)device;
        (void)desc;
        return nullptr;
    }
};

// === 资源工具函数 ===

/**
 * @brief 计算缓冲区对齐后的大小
 * @param size 原始大小
 * @param alignment 对齐要求
 * @return 对齐后的大小
 */
u64 AlignBufferSize(u64 size, u64 alignment) {
    if (alignment == 0) return size;
    return (size + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief 计算纹理的大小
 * @param desc 纹理描述符
 * @return 纹理大小（字节）
 */
u64 CalculateTextureSize(const TextureDesc& desc) {
    u64 size = 0;
    
    // 计算单个纹理层的大小
    u64 layerSize = 0;
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        u32 width = std::max(1u, desc.size.x >> mip);
        u32 height = std::max(1u, desc.size.y >> mip);
        u32 depth = std::max(1u, desc.size.z >> mip);
        
        u64 pixelSize = GetFormatSize(desc.format);
        u64 mipSize = static_cast<u64>(width) * height * depth * pixelSize;
        layerSize += mipSize;
    }
    
    // 乘以数组大小
    size = layerSize * desc.arraySize;
    
    return size;
}

/**
 * @brief 计算缓冲区描述符的大小
 * @param desc 缓冲区描述符
 * @return 缓冲区大小（字节）
 */
u64 CalculateBufferSize(const BufferDesc& desc) {
    u64 size = 0;
    
    switch (desc.type) {
        case BufferType::Vertex:
            size = static_cast<u64>(desc.vertex.vertexCount) * desc.vertex.vertexStride;
            break;
        case BufferType::Index:
            size = static_cast<u64>(desc.index.indexCount) * GetFormatSize(desc.index.format);
            break;
        case BufferType::Constant:
            size = desc.size;
            break;
        case BufferType::Structured:
            size = static_cast<u64>(desc.structured.elementCount) * desc.structured.elementStride;
            break;
        case BufferType::Raw:
            size = desc.size;
            break;
        case BufferType::AccelerationStructure:
            size = desc.size;
            break;
        default:
            size = desc.size;
            break;
    }
    
    return size;
}

/**
 * @brief 验证资源描述符
 * @param desc 资源描述符
 * @return 描述符是否有效
 */
bool ValidateResourceDesc(const ResourceDesc& desc) {
    if (desc.type == ResourceType::Unknown) return false;
    if (desc.usage == ResourceUsage::None) return false;
    if (desc.memoryUsage == GPUMemoryUsage::Unknown) return false;
    if (desc.size == 0) return false;
    
    // 检查用途和内存类型的兼容性
    switch (desc.memoryUsage) {
        case GPUMemoryUsage::Immutable:
            // 不可变资源不能有更新用途
            if (HasUsage(desc.usage, ResourceUsage::UnorderedAccess) ||
                HasUsage(desc.usage, ResourceUsage::CopyDest)) {
                return false;
            }
            break;
            
        case GPUMemoryUsage::Dynamic:
            // 动态资源通常用于CPU频繁更新
            if (!HasUsage(desc.usage, ResourceUsage::VertexBuffer) &&
                !HasUsage(desc.usage, ResourceUsage::IndexBuffer) &&
                !HasUsage(desc.usage, ResourceUsage::ConstantBuffer)) {
                // 不是错误，但可能不是最佳实践
            }
            break;
            
        case GPUMemoryUsage::Readback:
            // 回读资源只能作为复制目标
            if (desc.usage != ResourceUsage::CopyDest) {
                return false;
            }
            break;
            
        case GPUMemoryUsage::Staging:
            // 暂存资源用于数据传输
            if (desc.usage != ResourceUsage::CopySource && 
                desc.usage != ResourceUsage::CopyDest) {
                return false;
            }
            break;
            
        default:
            break;
    }
    
    return true;
}

/**
 * @brief 验证缓冲区描述符
 * @param desc 缓冲区描述符
 * @return 描述符是否有效
 */
bool ValidateBufferDesc(const BufferDesc& desc) {
    // 基础验证
    ResourceUsage usage = static_cast<ResourceUsage>(desc.bindFlags);
    if (!ValidateResourceDesc(ResourceDesc(ResourceType::Buffer, usage, desc.memoryUsage, 0, desc.name.c_str()))) {
        return false;
    }
    
    switch (desc.type) {
        case BufferType::Vertex:
            if (desc.vertex.vertexCount == 0 || desc.vertex.vertexStride == 0) return false;
            break;
            
        case BufferType::Index:
            if (desc.index.indexCount == 0) return false;
            if (desc.index.format != DataFormat::R16_UInt && 
                desc.index.format != DataFormat::R32_UInt) {
                return false;
            }
            break;
            
        case BufferType::Constant:
            if (desc.size == 0) return false;
            // 常量缓冲区通常需要对齐到256字节
            break;
            
        case BufferType::Structured:
            if (desc.structured.elementCount == 0 || desc.structured.elementStride == 0) return false;
            break;
            
        case BufferType::Raw:
            if (desc.size == 0) return false;
            break;
            
        default:
            break;
    }
    
    return true;
}

/**
 * @brief 验证纹理描述符
 * @param desc 纹理描述符
 * @return 描述符是否有效
 */
bool ValidateTextureDesc(const TextureDesc& desc) {
    // 基础验证 - 纹理的ResourceUsage根据TextureUsage推断
    ResourceUsage usage = ResourceUsage::None;
    if (static_cast<u32>(desc.usage) & static_cast<u32>(TextureUsage::ShaderResource)) {
        usage = usage | ResourceUsage::ShaderResource;
    }
    if (static_cast<u32>(desc.usage) & static_cast<u32>(TextureUsage::RenderTarget)) {
        usage = usage | ResourceUsage::RenderTarget;
    }
    if (static_cast<u32>(desc.usage) & static_cast<u32>(TextureUsage::DepthStencil)) {
        usage = usage | ResourceUsage::DepthStencil;
    }
    
    if (!ValidateResourceDesc(ResourceDesc(ResourceType::Texture, usage, desc.memoryUsage, 0, desc.name.c_str()))) {
        return false;
    }
    
    // 尺寸验证
    if (desc.size.x == 0) return false;
    
    switch (desc.type) {
        case TextureType::Texture1D:
            if (desc.size.y != 1 || desc.size.z != 1) return false;
            break;
            
        case TextureType::Texture2D:
            if (desc.size.y == 0 || desc.size.z != 1) return false;
            break;
            
        case TextureType::Texture3D:
            if (desc.size.y == 0 || desc.size.z == 0) return false;
            break;
            
        case TextureType::TextureCube:
            if (desc.size.y == 0 || desc.size.z != 6) return false;
            break;
            
        case TextureType::Texture1DArray:
            if (desc.size.y != 1 || desc.size.z != 1 || desc.arraySize == 0) return false;
            break;
            
        case TextureType::Texture2DArray:
            if (desc.size.y == 0 || desc.size.z != 1 || desc.arraySize == 0) return false;
            break;
            
        case TextureType::TextureCubeArray:
            if (desc.size.y == 0 || desc.size.z != 6 || desc.arraySize == 0) return false;
            break;
            
        default:
            return false;
    }
    
    // Mip层级验证
    if (desc.mipLevels == 0) return false;
    
    // 格式验证
    if (desc.format == DataFormat::Unknown) return false;
    
    return true;
}

// === 资源状态转换 ===

/**
 * @brief 获取状态转换的文本描述
 * @param from 起始状态
 * @param to 目标状态
 * @return 状态转换描述
 */
const char* GetStateTransitionName(ResourceState from, ResourceState to) {
    static const char* stateNames[] = {
        "Unknown", "Created", "Allocated", "PendingUpload", 
        "Ready", "InUse", "PendingDestroy", "Destroyed"
    };
    
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s -> %s", 
             stateNames[static_cast<int>(from)], 
             stateNames[static_cast<int>(to)]);
    return buffer;
}

/**
 * @brief 检查状态转换是否合法
 * @param from 起始状态
 * @param to 目标状态
 * @return 转换是否合法
 */
bool IsValidStateTransition(ResourceState from, ResourceState to) {
    // 状态转换规则表
    static const bool transitionTable[8][8] = {
        // From: Unknown, Created, Allocated, PendingUpload, Ready, InUse, PendingDestroy, Destroyed
        /* To: Unknown     */ {true,  false, false, false,   false, false, false, false},
        /*      Created    */ {true,  true,  false, false,   false, false, false, false},
        /*      Allocated  */ {true,  true,  true,  false,   false, false, false, false},
        /*      PendingUpload*/{true, true,  true,  true,    false, false, false, false},
        /*      Ready      */ {true,  true,  true,  true,    true,  false, false, false},
        /*      InUse      */ {true,  false, false, false,   true,  true,  false, false},
        /*      PendingDestroy*/{true, true,  true,  true,    true,  true,  true,  false},
        /*      Destroyed  */ {true,  true,  true,  true,    true,  true,  true,  true}
    };
    
    int fromIndex = static_cast<int>(from);
    int toIndex = static_cast<int>(to);
    
    if (fromIndex < 0 || fromIndex >= 8 || toIndex < 0 || toIndex >= 8) {
        return false;
    }
    
    return transitionTable[toIndex][fromIndex];
}

// === 资源调试 ===

/**
 * @brief 打印资源信息
 * @param resource 资源指针
 * @param verbose 是否输出详细信息
 */
void PrintResourceInfo(const RHIResource* resource, bool verbose) {
    if (!resource) {
        printf("Resource: nullptr\n");
        return;
    }
    
    printf("Resource Info:\n");
    printf("  Handle: 0x%016llx\n", static_cast<unsigned long long>(resource->GetHandle()));
    printf("  Type: %d\n", static_cast<int>(resource->GetType()));
    printf("  State: %d\n", static_cast<int>(resource->GetState()));
    printf("  Size: %llu bytes\n", static_cast<unsigned long long>(resource->GetSize()));
    printf("  RefCount: %u\n", resource->GetRefCount());
    printf("  Name: %s\n", resource->GetName());
    
    if (verbose) {
        printf("  Usage: 0x%08x\n", static_cast<u32>(resource->GetUsage()));
        printf("  IsValid: %s\n", resource->IsValid() ? "true" : "false");
        printf("  CanMap: %s\n", resource->CanMap() ? "true" : "false");
        printf("  CanUpdate: %s\n", resource->CanUpdate() ? "true" : "false");
        printf("  IsInUse: %s\n", resource->IsInUse() ? "true" : "false");
    }
}

/**
 * @brief 打印资源统计信息
 * @param stats 资源统计信息
 */
void PrintResourceStats(const ResourceStats& stats) {
    printf("Resource Statistics:\n");
    printf("  Buffers: %u (%llu bytes)\n", 
           stats.bufferCount, static_cast<unsigned long long>(stats.bufferMemoryUsage));
    printf("  Textures: %u (%llu bytes)\n", 
           stats.textureCount, static_cast<unsigned long long>(stats.textureMemoryUsage));
    printf("  Pipelines: %u\n", stats.pipelineCount);
    printf("  Total Memory: %llu bytes (%.2f MB)\n", 
           static_cast<unsigned long long>(stats.totalMemoryUsage),
           stats.totalMemoryUsage / (1024.0f * 1024.0f));
}

} // namespace primal::graphics::rhi