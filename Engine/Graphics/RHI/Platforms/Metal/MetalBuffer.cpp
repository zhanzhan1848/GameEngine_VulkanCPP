/**
 * @file MetalBuffer.cpp
 * @brief MetalBuffer 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 0.1.0
 */

#include "MetalBuffer.h"
#include "MetalDevice.h"
#include "../../Core/RHIAdaptiveMemoryPool.h"

namespace primal::graphics::rhi {

// 辅助函数：将 BufferType 转换为 ResourceUsage
static ResourceUsage GetResourceUsageFromBufferType(BufferType type, u32 bindFlags) {
    ResourceUsage usage = ResourceUsage::None;
    
    switch (type) {
        case BufferType::Vertex:
            usage = ResourceUsage::VertexBuffer;
            break;
        case BufferType::Index:
            usage = ResourceUsage::IndexBuffer;
            break;
        case BufferType::Constant:
            usage = ResourceUsage::ConstantBuffer;
            break;
        case BufferType::Structured:
            usage = ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess;
            break;
        case BufferType::Indirect:
            usage = ResourceUsage::IndirectArg;
            break;
        case BufferType::Raw:
             usage = ResourceUsage::ShaderResource | ResourceUsage::UnorderedAccess | ResourceUsage::CopySource | ResourceUsage::CopyDest;
             break;
        default:
            break;
    }
    
    // 如果有额外的绑定标志，可以在这里处理
    // 例如：bindFlags & BIND_UNORDERED_ACCESS -> usage |= ResourceUsage::UnorderedAccess
    
    return usage;
}

MetalBuffer::MetalBuffer(MetalDevice& device, const BufferDesc& desc)
    : RHIResource(device, ResourceDesc(
        ResourceType::Buffer,
        GetResourceUsageFromBufferType(desc.type, desc.bindFlags),
        desc.memoryUsage,
        desc.size,
        desc.name.c_str()
      ))
{
}

MetalBuffer::MetalBuffer(MetalBuffer&& other) noexcept
    : RHIResource(std::move(other)), mtlBuffer_(other.mtlBuffer_),
      heap_(other.heap_), heapOffset_(other.heapOffset_),
      pool_(other.pool_), poolHandle_(other.poolHandle_) {
    other.mtlBuffer_ = nullptr;
    other.heap_ = nullptr;
    other.heapOffset_ = 0;
    other.pool_ = nullptr;
    other.poolHandle_ = 0;
}

MetalBuffer& MetalBuffer::operator=(MetalBuffer&& other) noexcept {
    if (this != &other) {
        // 先销毁当前资源
        destroyImpl();
        // 移动基类
        RHIResource::operator=(std::move(other));
        // 移动成员
        mtlBuffer_ = other.mtlBuffer_;
        heap_ = other.heap_;
        heapOffset_ = other.heapOffset_;
        pool_ = other.pool_;
        poolHandle_ = other.poolHandle_;
        
        other.mtlBuffer_ = nullptr;
        other.heap_ = nullptr;
        other.heapOffset_ = 0;
        other.pool_ = nullptr;
        other.poolHandle_ = 0;
    }
    return *this;
}

MetalBuffer::~MetalBuffer() {
    destroyImpl();
}

bool MetalBuffer::Initialize() {
    if (desc_.size == 0) {
        std::cerr << "[MetalBuffer] Initialize failed: size is 0" << std::endl;
        return false;
    }

    MTL::ResourceOptions options = getResourceOptions();
    
    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MTL::Device* mtlDevice = metalDevice.GetNativeDevice();
    if (!mtlDevice) {
        std::cerr << "[MetalBuffer] Initialize failed: Native device is null" << std::endl;
        return false;
    }

    // 创建 Metal 缓冲区
    if (heap_) {
        mtlBuffer_ = heap_->newBuffer(desc_.size, options, heapOffset_);
    } else {
        mtlBuffer_ = mtlDevice->newBuffer(desc_.size, options);
    }
    
    if (!mtlBuffer_) {
        std::cerr << "[MetalBuffer] Failed to create buffer: " << GetName() 
                  << " Size: " << desc_.size 
                  << " Options: " << options 
                  << (heap_ ? " (From Heap)" : "") << std::endl;
        return false;
    }

    // 设置调试名称
    if (desc_.name) {
        NS::String* name = NS::String::string(desc_.name, NS::UTF8StringEncoding);
        mtlBuffer_->setLabel(name);
    }

    // CRITICAL FIX: Zero-initialize StorageModeShared buffers.
    // Metal's newBuffer/newBufferFromHeap do NOT guarantee zero-initialized memory.
    // For GPU-driven pipelines with triple buffering, the first frame reads from a
    // buffer slot that hasn't been written to yet by the GPU. Garbage data in that
    // buffer causes non-deterministic rendering (some geometry disappears) that
    // varies between program runs due to ASLR affecting heap address layout.
    // Only zero Shared-mode buffers (CPU/GPU coherent) since Private buffers
    // are not CPU-accessible and must be written via GPU commands.
    if (options & MTL::ResourceStorageModeShared) {
        void* contents = mtlBuffer_->contents();
        if (contents) {
            memset(contents, 0, desc_.size);
        }
    }

    state_ = ResourceState::Ready;
    // std::cout << "[MetalBuffer] Initialized at address: " << this << " Handle: " << handle_ << std::endl;
    return true;
}

void MetalBuffer::destroyImpl() {
    auto mtlBuffer = mtlBuffer_;
    auto pool = pool_;
    auto poolHandle = poolHandle_;

    if (mtlBuffer || (pool && poolHandle != 0)) {
        // std::cout << "[MetalBuffer] Destroying buffer at " << mtlBuffer << std::endl;
        device_.GetGarbageCollector().DeferredDestroy([mtlBuffer, pool, poolHandle]() {
            if (mtlBuffer) {
                mtlBuffer->release();
            }
            if (pool && poolHandle != 0) {
                pool->Deallocate(poolHandle);
            }
        });
    }

    mtlBuffer_ = nullptr;
    pool_ = nullptr;
    poolHandle_ = 0;
    heap_ = nullptr;
    heapOffset_ = 0;
}

void MetalBuffer::SetHeapAllocation(MTL::Heap* heap, u64 offset, RHIAdaptiveMemoryPool* pool, u32 handle) {
    heap_ = heap;
    heapOffset_ = offset;
    pool_ = pool;
    poolHandle_ = handle;
}

void* MetalBuffer::mapImpl(u64 offset, u64 size) {
    if (!mtlBuffer_) {
        std::cerr << "[MetalBuffer] mapImpl failed: mtlBuffer_ is null" << std::endl;
        return nullptr;
    }
    
    // 检查缓冲区是否可映射
    if (mtlBuffer_->storageMode() == MTL::StorageModePrivate) {
        std::cerr << "[MetalBuffer] Cannot map private buffer: " << GetName() << std::endl;
        return nullptr;
    }
    
    u8* bufferContents = static_cast<u8*>(mtlBuffer_->contents());
    if (!bufferContents) {
         static bool loggedMapFail = false;
         if (!loggedMapFail) {
             std::cerr << "[MetalBuffer] mapImpl failed: contents() returned null. StorageMode: " 
                       << (int)mtlBuffer_->storageMode() 
                       << " Size: " << desc_.size
                       << " Name: " << GetName() << std::endl;
             loggedMapFail = true;
         }
         return nullptr;
    }
    
    return bufferContents + offset;
}

void MetalBuffer::unmapImpl() {
    if (!mtlBuffer_) return;
    
    // 对于 Managed 模式，需要通知 Metal 数据已修改
    // Shared 模式在 Apple Silicon 上是 Coherent 的，但为了保险起见（以及兼容 Intel Mac），也进行通知
#if defined(__APPLE__) || defined(__MAC_OS_X_VERSION_MAX_ALLOWED)
        if (mtlBuffer_->storageMode() == MTL::StorageModeManaged) {
            mtlBuffer_->didModifyRange(NS::Range(0, desc_.size));
        }
#endif
}

bool MetalBuffer::updateDataImpl(const void* data, u64 size, u64 offset) {
    if (!mtlBuffer_ || !data) {
        return false;
    }
    if (offset + size > desc_.size) {
        return false;
    }

    // 尝试直接映射更新
    if (CanMap()) {
        void* ptr = mapImpl(offset, size);
        if (ptr) {
            memcpy(ptr, data, size);
            unmapImpl();
            
            static bool loggedFastPath = false;
                    if (!loggedFastPath) {
                        // Safe log: avoid calling GetName() if potential risk, just log address or skip name
                        std::cerr << "[MetalBuffer] updateDataImpl used Fast Path (Map) for buffer at " << this << std::endl;
                        loggedFastPath = true;
                    }
                    return true;
                }
            }

            // 如果不可映射（如 Private 存储模式），使用 Staging Buffer
            static bool loggedSlowPath = false;
            if (!loggedSlowPath) {
                std::cerr << "[MetalBuffer] updateDataImpl used SLOW Path (Staging) for buffer at " << this
                          << " CanMap: " << CanMap() 
                          << " MemoryUsage: " << (int)desc_.memoryUsage 
                          << std::endl;
                loggedSlowPath = true;
            }

    MetalDevice& metalDevice = static_cast<MetalDevice&>(device_);
    MTL::Device* mtlDevice = metalDevice.GetNativeDevice();
    MTL::CommandQueue* transferQueue = metalDevice.GetTransferQueue();

    if (!mtlDevice || !transferQueue) {
        std::cerr << "[MetalBuffer] updateDataImpl failed: mtlDevice or transferQueue is null. Device: " << mtlDevice << " Queue: " << transferQueue << std::endl;
        return false;
    }

    // 1. 创建暂存缓冲区 (Shared Mode)
    MTL::Buffer* stagingBuffer = mtlDevice->newBuffer(size, MTL::ResourceStorageModeShared);
    if (!stagingBuffer) {
        std::cerr << "[MetalBuffer] updateDataImpl failed: Failed to create staging buffer. Size: " << size << std::endl;
        return false;
    }

    // 2. 将数据拷贝到暂存缓冲区
    memcpy(stagingBuffer->contents(), data, size);
    
    // 3. 创建命令缓冲区和Blit编码器
    MTL::CommandBuffer* cmdBuffer = transferQueue->commandBuffer();
    if (!cmdBuffer) {
        std::cerr << "[MetalBuffer] updateDataImpl failed: Failed to create command buffer" << std::endl;
        stagingBuffer->release();
        return false;
    }

    MTL::BlitCommandEncoder* blitEncoder = cmdBuffer->blitCommandEncoder();
    if (!blitEncoder) {
        std::cerr << "[MetalBuffer] updateDataImpl failed: Failed to create blit encoder" << std::endl;
        stagingBuffer->release();
        return false;
    }

    // 4. 编码拷贝命令
    blitEncoder->copyFromBuffer(stagingBuffer, 0, mtlBuffer_, offset, size);
    blitEncoder->endEncoding();

    // 5. 提交并等待完成
    cmdBuffer->commit();
    cmdBuffer->waitUntilCompleted();

    // 6. 释放暂存缓冲区
    stagingBuffer->release();

    return true;
}

MTL::ResourceOptions MetalBuffer::getResourceOptions() const {
    MTL::ResourceOptions options = 0;
    
    // 根据内存使用模式设置 ResourceOptions
    switch (desc_.memoryUsage) {
        case GPUMemoryUsage::Dynamic:
            // 动态数据：CPU 频繁读写
            // 强制使用 Shared 模式，避免 Managed 模式下的同步问题
            options |= MTL::ResourceStorageModeShared;
            options |= MTL::ResourceCPUCacheModeDefaultCache;
            break;

        case GPUMemoryUsage::Staging:
        case GPUMemoryUsage::Readback:
            // 动态数据：CPU 频繁读写
            // Apple Silicon: Shared
            // Intel/Discrete: Managed (或者 Shared)
            // 这里为了简化，统一使用 Shared，它在所有 macOS 平台都支持且行为符合预期（Unified Memory）
            options |= MTL::ResourceStorageModeShared;
            options |= MTL::ResourceCPUCacheModeDefaultCache;
            break;
            
        case GPUMemoryUsage::Static:
        case GPUMemoryUsage::Immutable:
            // 静态数据：GPU 只读，CPU 初始化一次
            // 使用 Private 存储模式，通过 Staging Buffer 上传
            options |= MTL::ResourceStorageModePrivate;
            break;
            
        default:
            options |= MTL::ResourceStorageModeShared;
            break;
    }
    
    return options;
}

} // namespace primal::graphics::rhi
