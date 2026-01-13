/**
 * @file MetalDevice.cpp
 * @brief MetalDevice 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 0.1.0
 */

#include "MetalDevice.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalCommandBuffer.h"
#include "MetalPipeline.h"
#include "MetalShader.h"
#include "MetalSampler.h"
#include "MetalDescriptorSet.h"
#include "MetalSwapChain.h"
#include "MetalQuery.h"
#include "MetalSync.h"
#include "MetalRenderPass.h"
#include <iostream>
#include <dispatch/dispatch.h>

namespace primal::graphics::rhi {

MetalDevice::MetalDevice(const DeviceDesc& desc)
    : RHIDevice(desc) {
}

MetalDevice::~MetalDevice() {
    Shutdown();
}

bool MetalDevice::initializeImpl() {
    // 1. 创建 Metal 设备
    mtlDevice_ = MTL::CreateSystemDefaultDevice();
    if (!mtlDevice_) {
        std::cerr << "[Metal] Failed to create system default device." << std::endl;
        return false;
    }

    // 2. 创建命令队列
    graphicsQueue_ = mtlDevice_->newCommandQueue();
    if (!graphicsQueue_) {
        std::cerr << "[Metal] Failed to create graphics command queue." << std::endl;
        mtlDevice_->release();
        mtlDevice_ = nullptr;
        return false;
    }
    
    // 简单起见，所有队列都创建。在实际引擎中可能根据需求延迟创建或复用。
    computeQueue_ = mtlDevice_->newCommandQueue();
    transferQueue_ = mtlDevice_->newCommandQueue();

    initializeMemoryPool();

    // 预分配资源以避免多线程扩容导致指针失效
    // 尤其是 CommandBuffer，在多线程渲染中非常关键
    // 同时分配其他资源以防止 Resize 导致的指针失效 (因为 ResourceManager 缓存了指针)
    commandBufferAllocator_.Reserve(256);
    bufferAllocator_.Reserve(1024);
    textureAllocator_.Reserve(512);
    pipelineAllocator_.Reserve(256);
    descriptorSetAllocator_.Reserve(1024);
    descriptorSetLayoutAllocator_.Reserve(128);
    pipelineLayoutAllocator_.Reserve(128);
    shaderAllocator_.Reserve(256);
    samplerAllocator_.Reserve(64);
    renderPassAllocator_.Reserve(64);
    syncAllocator_.Reserve(64);
    queryPoolAllocator_.Reserve(16);

    std::cout << "[MetalDevice] Allocators reserved. Buffers: 1024, Textures: 512" << std::endl;

    return true;
}

void MetalDevice::shutdownImpl() {
    // std::cout << "[MetalDevice] Shutdown started." << std::endl;
    // 1. 清理所有延迟销毁的资源 (必须在 Allocator Shutdown 之前，否则会导致 Double Free)
    // std::cout << "[MetalDevice] Shutting down GC..." << std::endl;
    gc_.Shutdown();

    // 2. 释放所有分配的资源
    // std::cout << "[MetalDevice] Shutting down allocators..." << std::endl;
    // std::cout << "  CommandBuffer..." << std::endl; 
    commandBufferAllocator_.Shutdown();
    // std::cout << "  Buffer..." << std::endl; 
    bufferAllocator_.Shutdown();
    // std::cout << "  Texture..." << std::endl; 
    textureAllocator_.Shutdown();
    // std::cout << "  Sync..." << std::endl; 
    syncAllocator_.Shutdown();
    // std::cout << "  QueryPool..." << std::endl; 
    queryPoolAllocator_.Shutdown();
    // std::cout << "  Shader..." << std::endl; 
    shaderAllocator_.Shutdown();
    // std::cout << "  Pipeline..." << std::endl; 
    pipelineAllocator_.Shutdown();
    // std::cout << "  PipelineLayout..." << std::endl; 
    pipelineLayoutAllocator_.Shutdown();
    // std::cout << "  Sampler..." << std::endl; 
    samplerAllocator_.Shutdown();
    // std::cout << "  DescriptorSetLayout..." << std::endl; 
    descriptorSetLayoutAllocator_.Shutdown();
    // std::cout << "  DescriptorSet..." << std::endl; 
    descriptorSetAllocator_.Shutdown();

    // 3. 再次清理 GC，处理 Allocator Shutdown 产生的新垃圾 (关键修复：防止 MemoryPool 销毁后 GC 回调访问悬空指针)
    // std::cout << "[MetalDevice] Shutting down GC (Pass 2)..." << std::endl;
    gc_.Shutdown();

    // 4. 销毁内存池
    // std::cout << "[MetalDevice] Shutting down memory pool..." << std::endl;
    shutdownMemoryPool();
    // std::cout << "[MetalDevice] Shutdown finished." << std::endl;


    if (transferQueue_) {
        transferQueue_->release();
        transferQueue_ = nullptr;
    }
    if (computeQueue_) {
        computeQueue_->release();
        computeQueue_ = nullptr;
    }
    if (graphicsQueue_) {
        graphicsQueue_->release();
        graphicsQueue_ = nullptr;
    }
    if (mtlDevice_) {
        mtlDevice_->release();
        mtlDevice_ = nullptr;
    }
}

void MetalDevice::waitIdleImpl() const {
    auto waitQueue = [](MTL::CommandQueue* queue) {
        if (queue) {
            MTL::CommandBuffer* cmdBuf = queue->commandBuffer();
            if (cmdBuf) {
                cmdBuf->commit();
                cmdBuf->waitUntilCompleted();
            }
        }
    };

    waitQueue(graphicsQueue_);
    waitQueue(computeQueue_);
    waitQueue(transferQueue_);
}

void MetalDevice::beginFrameImpl() {
    // 帧开始逻辑
}

void MetalDevice::endFrameImpl() {
    currentFrameIndex_ = (currentFrameIndex_ + 1) % desc_.maxFramesInFlight;
}

void MetalDevice::presentImpl() {
    // 呈现逻辑
}

void MetalDevice::queryDeviceInfo(DeviceInfo& info) {
    if (!mtlDevice_) return;
    
    info.platform = RHIPlatform::Metal;
    
    NS::String* name = mtlDevice_->name();
    if (name) {
        strncpy(info.deviceName, name->utf8String(), sizeof(info.deviceName) - 1);
    }
    
    // 获取当前已分配显存大小
    // 注意：Metal API 并不直接提供"总显存"，currentAllocatedSize返回的是当前应用已使用的显存
    // 这里仅作为示例
    info.dedicatedVideoMemory = mtlDevice_->currentAllocatedSize();
    
    // Metal 3 check (Apple7 family)
    if (mtlDevice_->supportsFamily(MTL::GPUFamilyApple7)) {
        info.supportsRayTracing = true;
        info.supportsMeshShaders = true;
    }
    
    // 设置一些合理的默认值
    info.maxTexture2DSize = 16384;
    info.maxTexture3DSize = 2048;
}

uint32_t MetalDevice::getCurrentFrameIndexImpl() const {
    return currentFrameIndex_;
}

MetalBuffer* MetalDevice::GetBuffer(ResourceHandle handle) {
    return bufferAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalTexture* MetalDevice::GetTexture(ResourceHandle handle) {
    return textureAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalCommandBuffer* MetalDevice::GetCommandBuffer(CommandBufferHandle handle) {
    return commandBufferAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalSync* MetalDevice::GetSync(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return nullptr;
    return syncAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalQueryPool* MetalDevice::GetQueryPool(QueryPoolHandle handle) {
    return queryPoolAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalShader* MetalDevice::GetShader(ShaderHandle handle) {
    return shaderAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalPipeline* MetalDevice::GetPipeline(PipelineHandle handle) {
    return pipelineAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalSampler* MetalDevice::GetSampler(SamplerHandle handle) {
    return samplerAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalRenderPass* MetalDevice::GetRenderPass(RenderPassHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return nullptr;
    return renderPassAllocator_.Get(handle);
}

void MetalDevice::RegisterPipelineDependency(PipelineHandle pipeline, ShaderHandle shader) {
    if (shader != handles::INVALID_SHADER) {
        std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
        shaderToPipelines_[shader].push_back(pipeline);
    }
}

void MetalDevice::UnregisterPipelineDependency(PipelineHandle pipeline) {
    std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
    for (auto& pair : shaderToPipelines_) {
        auto& pipelines = pair.second;
        for (size_t i = 0; i < pipelines.size(); ++i) {
            if (pipelines[i] == pipeline) {
                pipelines.erase_unordered(i);
                break;
            }
        }
    }
}

bool MetalDevice::ReloadShader(ShaderHandle shaderHandle, const void* data, size_t size) {
    MetalShader* shader = GetShader(shaderHandle);
    if (!shader) return false;
    
    // Reload shader module
    if (!shader->Reload(data, size)) {
        std::cerr << "[MetalDevice] Failed to reload shader: " << shaderHandle << std::endl;
        return false;
    }
    
    // Recreate dependent pipelines
    std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
    auto it = shaderToPipelines_.find(shaderHandle);
    if (it != shaderToPipelines_.end()) {
        for (PipelineHandle pipelineHandle : it->second) {
            MetalPipeline* pipeline = GetPipeline(pipelineHandle);
            if (pipeline) {
                if (!pipeline->Recreate()) {
                    std::cerr << "[MetalDevice] Failed to recreate pipeline: " << pipelineHandle 
                              << " during shader reload." << std::endl;
                } else {
                    std::cout << "[MetalDevice] Recreated pipeline: " << pipelineHandle << std::endl;
                }
            }
        }
    }
    
    return true;
}

// === CRTP 实现接口 ===

bool MetalDevice::submitImpl(const QueueSubmitInfo& info) {
    MetalCommandBuffer* cmdBuf = commandBufferAllocator_.Get(static_cast<uint32_t>(info.cmdBuffer));
    if (cmdBuf) {
        if (info.waitSemaphore != handles::INVALID_SYNC) {
            // Default value 1 for binary semaphore simulation if not specified
            cmdBuf->AddWaitSemaphore(info.waitSemaphore, 1);
        }
        if (info.signalSemaphore != handles::INVALID_SYNC) {
            cmdBuf->AddSignalSemaphore(info.signalSemaphore, 1);
        }
        
        if (info.signalFence != handles::INVALID_SYNC) {
            MetalSync* sync = GetSync(info.signalFence);
            if (sync && sync->GetNativeEvent()) {
                 uint64_t nextVal = sync->GetValue() + 1;
                 // 直接访问 MetalCommandBuffer 的私有成员，因为是 friend
                 if (cmdBuf->mtlCommandBuffer_) {
                     cmdBuf->mtlCommandBuffer_->encodeSignalEvent(sync->GetNativeEvent(), nextVal);
                     sync->SetValue(nextVal);
                 }
            }
        }
        
        return cmdBuf->Submit();
    }
    return false; 
}

SyncHandle MetalDevice::createSyncImpl() {
    uint32_t id = syncAllocator_.Allocate(mtlDevice_);
    return SyncHandle(id);
}



bool MetalDevice::waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
    MetalSync* sync = GetSync(handle);
    if (!sync) return false;
    
    // Metal SharedEvent 等待通常需要通过 CommandBuffer 提交 wait 命令，
    // 或者使用 waitUntilSignaledValue (CPU 等待)
    MTL::SharedEvent* event = sync->GetNativeEvent();
    if (!event) return false;

    // 获取当前期望的目标值 (假设由 Submit 递增)
    uint64_t value = sync->GetValue();
    if (value == 0) return true; // 初始状态，视为已完成

    // 使用 dispatch_semaphore 实现同步等待
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    dispatch_retain(sema); // Retain for the block
    
    MTL::SharedEventListener* listener = MTL::SharedEventListener::alloc()->init();
    
    // 注册监听器
    event->notifyListener(listener, value, ^(MTL::SharedEvent* evt, uint64_t val) {
        dispatch_semaphore_signal(sema);
        dispatch_release(sema);
    });
    
    // 等待信号量
    long result = dispatch_semaphore_wait(sema, dispatch_time(DISPATCH_TIME_NOW, timeoutMs * NSEC_PER_MSEC));
    
    listener->release();
    dispatch_release(sema);
    
    return result == 0;
}

void MetalDevice::initializeMemoryPool() {
    if (!mtlDevice_) return;
    
    // 创建 Heap (256MB Shared)
    // 注意：实际应用中应该根据显存大小和需求动态配置
    MTL::HeapDescriptor* heapDesc = MTL::HeapDescriptor::alloc()->init();
    heapDesc->setSize(256 * 1024 * 1024); // 256MB
    heapDesc->setStorageMode(MTL::StorageModeShared);
    heapDesc->setCpuCacheMode(MTL::CPUCacheModeDefaultCache);
    // 使用 Placement 堆，因为 RHIAdaptiveMemoryPool 会手动管理内存块和偏移
    heapDesc->setType(MTL::HeapTypePlacement);
    
    heap_ = mtlDevice_->newHeap(heapDesc);
    heapDesc->release();
    
    if (heap_) {
        // 创建 RHIAdaptiveMemoryPool
        MemoryPoolDesc poolDesc;
        poolDesc.poolSize = 256 * 1024 * 1024;
        poolDesc.blockSize = 256; // 最小块大小
        poolDesc.name = "MetalSharedPool";
        
        memoryPool_ = new RHIAdaptiveMemoryPool(*this, poolDesc);
        if (memoryPool_->Initialize()) {
            std::cout << "[MetalDevice] Memory Pool Initialized (256MB Shared)" << std::endl;
        } else {
            std::cerr << "[MetalDevice] Failed to initialize Memory Pool" << std::endl;
        }
    } else {
        std::cerr << "[MetalDevice] Failed to create MTLHeap" << std::endl;
    }
}

void MetalDevice::shutdownMemoryPool() {
    if (memoryPool_) {
        memoryPool_->Destroy();
        delete memoryPool_;
        memoryPool_ = nullptr;
    }
    if (heap_) {
        heap_->release();
        heap_ = nullptr;
    }
}

ResourceHandle MetalDevice::createBufferImpl(const BufferDesc& desc) {
    uint32_t id = bufferAllocator_.Allocate(*this, desc);
    MetalBuffer* buffer = bufferAllocator_.Get(id);
    if (buffer) {
        // 尝试从内存池分配 (仅针对 Shared/Dynamic/Staging 内存)
        // Static 内存通常使用 Private 模式，需要单独的 Heap 或者独立分配
        if (memoryPool_ && heap_) {
             bool usePool = false;
             // 根据 MetalBuffer::getResourceOptions 的逻辑，Dynamic, Staging, Readback 都是 Shared
             if (desc.memoryUsage == GPUMemoryUsage::Dynamic || 
                 desc.memoryUsage == GPUMemoryUsage::Staging || 
                 desc.memoryUsage == GPUMemoryUsage::Readback) {
                 
                 // 检查大小是否适合池分配 (留一点余量或者限制最大分配)
                 if (desc.size <= memoryPool_->GetDesc().poolSize) {
                     u32 handle = memoryPool_->Allocate(desc.size, 256, desc.memoryUsage);
                     if (handle != 0) {
                         MemoryBlock block = memoryPool_->GetMemoryBlock(handle);
                         buffer->SetHeapAllocation(heap_, block.offset, memoryPool_, handle);
                         usePool = true;
                     }
                 }
             }
        }

        if (buffer->Initialize()) {
            buffer->SetHandle(ResourceHandle(id));
            ResourceManager::Instance().RegisterResource(buffer);
            // std::cerr << "[MetalDevice] Created Buffer - ID: " << id << " Address: " << buffer << std::endl;
            return ResourceHandle(id);
        } else {
             std::cerr << "[MetalDevice] Buffer Initialize failed for id: " << id << std::endl;
        }
    } else {
        std::cerr << "[MetalDevice] Buffer allocation failed" << std::endl;
    }
    bufferAllocator_.Free(id);
    return handles::INVALID_RESOURCE;
}

ResourceHandle MetalDevice::createTextureImpl(const TextureDesc& desc) {
    uint32_t id = textureAllocator_.Allocate(*this, desc);
    MetalTexture* texture = textureAllocator_.Get(id);
    if (texture) {
        if (texture->Initialize()) {
            texture->SetHandle(ResourceHandle(id));
            return ResourceHandle(id);
        } else {
            std::cerr << "[MetalDevice] Texture Initialize failed for id: " << id << std::endl;
        }
    } else {
        std::cerr << "[MetalDevice] Texture allocation failed" << std::endl;
    }
    textureAllocator_.Free(id);
    return handles::INVALID_RESOURCE; 
}

ShaderHandle MetalDevice::createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
    uint32_t id = shaderAllocator_.Allocate(*this, data, size, stage, entryPoint);
    MetalShader* shader = shaderAllocator_.Get(id);
    if (shader && shader->Initialize()) {
        shader->SetHandle(ShaderHandle(id));
        return ShaderHandle(id);
    }
    shaderAllocator_.Free(id);
    return handles::INVALID_SHADER;
}

PipelineHandle MetalDevice::createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
    uint32_t id = pipelineAllocator_.Allocate(*this);
    MetalPipeline* pipeline = pipelineAllocator_.Get(id);
    if (pipeline && pipeline->Initialize(desc)) {
        pipeline->SetHandle(PipelineHandle(id));
        
        // 注册管线对Shader的依赖，用于热更新
        if (desc.vertexShader != handles::INVALID_SHADER) {
            RegisterPipelineDependency(PipelineHandle(id), desc.vertexShader);
        }
        if (desc.pixelShader != handles::INVALID_SHADER) {
            RegisterPipelineDependency(PipelineHandle(id), desc.pixelShader);
        }
        
        return PipelineHandle(id);
    }
    pipelineAllocator_.Free(id);
    return handles::INVALID_PIPELINE;
}

PipelineHandle MetalDevice::createComputePipelineImpl(const ComputePipelineDesc& desc) {
    uint32_t id = pipelineAllocator_.Allocate(*this);
    MetalPipeline* pipeline = pipelineAllocator_.Get(id);
    if (pipeline && pipeline->Initialize(desc)) {
        pipeline->SetHandle(PipelineHandle(id));
        
        // 注册管线对Shader的依赖，用于热更新
        if (desc.computeShader != handles::INVALID_SHADER) {
            RegisterPipelineDependency(PipelineHandle(id), desc.computeShader);
        }
        
        return PipelineHandle(id);
    }
    pipelineAllocator_.Free(id);
    return handles::INVALID_PIPELINE;
}

SamplerHandle MetalDevice::createSamplerImpl(const SamplerDesc& desc) {
    uint32_t id = samplerAllocator_.Allocate(*this);
    MetalSampler* sampler = samplerAllocator_.Get(id);
    if (sampler && sampler->Initialize(desc)) {
        sampler->SetHandle(SamplerHandle(id));
        return SamplerHandle(id);
    }
    samplerAllocator_.Free(id);
    return handles::INVALID_SAMPLER;
}

DescriptorSetLayoutHandle MetalDevice::createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) {
    uint32_t id = descriptorSetLayoutAllocator_.Allocate(*this, desc);
    MetalDescriptorSetLayout* layout = descriptorSetLayoutAllocator_.Get(id);
    if (layout && layout->Initialize()) {
        return DescriptorSetLayoutHandle(id);
    }
    descriptorSetLayoutAllocator_.Free(id);
    return static_cast<DescriptorSetLayoutHandle>(handles::INVALID_RESOURCE);
}

PipelineLayoutHandle MetalDevice::createPipelineLayoutImpl(const PipelineLayoutDesc& desc) {
    uint32_t id = pipelineLayoutAllocator_.Allocate(*this, desc);
    MetalPipelineLayout* layout = pipelineLayoutAllocator_.Get(id);
    if (layout && layout->Initialize()) {
        return PipelineLayoutHandle(id);
    }
    pipelineLayoutAllocator_.Free(id);
    return static_cast<PipelineLayoutHandle>(handles::INVALID_PIPELINE_LAYOUT);
}

DescriptorSetHandle MetalDevice::createDescriptorSetImpl(const DescriptorSetDesc& desc) {
    uint32_t id = descriptorSetAllocator_.Allocate(*this, desc);
    MetalDescriptorSet* set = descriptorSetAllocator_.Get(id);
    if (set && set->Initialize()) {
        return DescriptorSetHandle(id);
    }
    descriptorSetAllocator_.Free(id);
    return static_cast<DescriptorSetHandle>(handles::INVALID_RESOURCE);
}

void MetalDevice::updateDescriptorSetsImpl(uint32_t writeCount, const WriteDescriptorSet* writes) {
    // Group writes by descriptor set
    // Note: Since WriteDescriptorSet contains the dstSet handle, we can process them one by one
    // or group them. The MetalDescriptorSet::Update takes an array, but it expects them to be for 'this' set?
    // Let's check MetalDescriptorSet::Update signature.
    // void Update(uint32_t writeCount, const WriteDescriptorSet* writes);
    // It seems it iterates and checks write.dstSet? No, typically Update is called on the set instance
    // and the writes passed to it are for that set. 
    // BUT, the RHI interface is UpdateDescriptorSets(count, writes), where writes can target DIFFERENT sets.
    // So we need to iterate and dispatch.
    
    for (uint32_t i = 0; i < writeCount; ++i) {
        const WriteDescriptorSet& write = writes[i];
        MetalDescriptorSet* set = GetDescriptorSet(write.dstSet);
        if (set) {
            // We pass a single write to the set
            set->Update(&write, 1);
        }
    }
}

CommandBufferHandle MetalDevice::createCommandBufferImpl(CommandQueueType type) {
    uint32_t id = commandBufferAllocator_.Allocate(*this, type);
    MetalCommandBuffer* cmdBuf = commandBufferAllocator_.Get(id);
    if (cmdBuf) {
        if (cmdBuf->Initialize()) {
            // Register with global manager
            RegisterCommandBuffer(cmdBuf);
            return CommandBufferHandle(id);
        } else {
            std::cerr << "[MetalDevice] CommandBuffer Initialize failed for id: " << id << std::endl;
        }
    } else {
        std::cerr << "[MetalDevice] CommandBuffer allocation failed" << std::endl;
    }
    commandBufferAllocator_.Free(id);
    return handles::INVALID_COMMAND_BUFFER; 
}

void MetalDevice::destroyBufferImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    // std::cout << "[MetalDevice] Destroying Buffer - ID: " << (uint32_t)handle << std::endl;
    ResourceManager::Instance().UnregisterResource(handle);
    bufferAllocator_.Free(static_cast<uint32_t>(handle));
}

void* MetalDevice::mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) {
    MetalBuffer* buffer = GetBuffer(handle);
    if (!buffer) return nullptr;
    
    // 如果 size 为 0，则映射从 offset 到缓冲区末尾
    if (size == 0) {
        size = buffer->GetDesc().size - offset;
    }
    
    // 获取底层 MTLBuffer
    MTL::Buffer* mtlBuffer = buffer->GetNativeBuffer();
    if (!mtlBuffer) return nullptr;
    
    // 获取缓冲区内容指针并偏移
    uint8_t* ptr = static_cast<uint8_t*>(mtlBuffer->contents());
    if (!ptr) return nullptr;
    
    return ptr + offset;
}

void MetalDevice::unmapBufferImpl(ResourceHandle handle) {
    MetalBuffer* buffer = GetBuffer(handle);
    if (!buffer) return;
    
    MTL::Buffer* mtlBuffer = buffer->GetNativeBuffer();
    if (!mtlBuffer) return;
    
#if defined(PRIMAL_PLATFORM_MACOS)
    // 如果缓冲区是 Managed 模式，需要通知 Metal 修改了范围
    if (mtlBuffer->storageMode() == MTL::StorageModeManaged) {
        mtlBuffer->didModifyRange(NS::Range::Make(0, mtlBuffer->length()));
    }
#endif
}

void MetalDevice::destroyTextureImpl(ResourceHandle handle) {
    ResourceManager::Instance().UnregisterResource(handle);
    textureAllocator_.Free(static_cast<uint32_t>(handle));
}
void MetalDevice::destroyShaderImpl(ShaderHandle handle) {
    shaderAllocator_.Free(static_cast<uint32_t>(handle));
}
void MetalDevice::destroyPipelineImpl(PipelineHandle handle) {
    UnregisterPipelineDependency(handle);
    pipelineAllocator_.Free(static_cast<uint32_t>(handle));
}
void MetalDevice::destroySamplerImpl(SamplerHandle handle) {
    samplerAllocator_.Free(static_cast<uint32_t>(handle));
}
void MetalDevice::destroyCommandBufferImpl(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return;
    
    // Unregister from global manager
    UnregisterCommandBuffer(handle);
    
    commandBufferAllocator_.Free(static_cast<uint32_t>(handle));
}

void MetalDevice::destroySyncImpl(SyncHandle handle) {
    MetalSync* sync = syncAllocator_.Get(static_cast<uint32_t>(handle));
    if (sync) {
        MTL::SharedEvent* event = sync->DetachNativeEvent();
        if (event) {
            gc_.DeferredDestroy([event]() {
                event->release();
            });
        }
    }
    syncAllocator_.Free(static_cast<uint32_t>(handle));
}

QueryPoolHandle MetalDevice::createQueryPoolImpl(const QueryPoolDesc& desc) {
    MetalQueryType metalType;
    switch (desc.type) {
        case QueryType::Timestamp:
            metalType = MetalQueryType::Timestamp;
            break;
        case QueryType::Occlusion:
            metalType = MetalQueryType::Occlusion;
            break;
        case QueryType::PipelineStatistics:
            metalType = MetalQueryType::Statistics;
            break;
        default:
            return handles::INVALID_QUERY_POOL;
    }

    uint32_t id = queryPoolAllocator_.Allocate(mtlDevice_, metalType, desc.queryCount);
    return QueryPoolHandle(id);
}

void MetalDevice::destroyQueryPoolImpl(QueryPoolHandle handle) {
    queryPoolAllocator_.Free(static_cast<uint32_t>(handle));
}

RHISwapChain* MetalDevice::createSwapChainImpl(const SwapChainDesc& desc) {
    MetalSwapChain* swapChain = new MetalSwapChain(*this, desc);
    if (swapChain && swapChain->Initialize()) {
        return swapChain;
    }
    if (swapChain) {
        delete swapChain;
    }
    return nullptr;
}

void MetalDevice::destroySwapChainImpl(RHISwapChain* swapChain) {
    if (swapChain) {
        delete swapChain;
    }
}

MetalDescriptorSet* MetalDevice::GetDescriptorSet(DescriptorSetHandle handle) {
    return descriptorSetAllocator_.Get(static_cast<uint32_t>(handle));
}

MetalDescriptorSetLayout* MetalDevice::GetDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    return descriptorSetLayoutAllocator_.Get(static_cast<uint32_t>(handle));
}

void MetalDevice::destroyDescriptorSetImpl(DescriptorSetHandle handle) {
    descriptorSetAllocator_.Free(static_cast<uint32_t>(handle));
}

void MetalDevice::destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {
    descriptorSetLayoutAllocator_.Free(static_cast<uint32_t>(handle));
}

void MetalDevice::destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {
    pipelineLayoutAllocator_.Free(static_cast<uint32_t>(handle));
}

RenderPassHandle MetalDevice::createRenderPassImpl(const RenderPassDesc& desc) {
    RenderPassHandle handle = renderPassAllocator_.Allocate(*this, desc);
    if (handle == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;

    MetalRenderPass* pass = renderPassAllocator_.Get(handle);
    if (!pass || !pass->Initialize()) {
        renderPassAllocator_.Free(handle);
        return handles::INVALID_RESOURCE;
    }

    return handle;
}

void MetalDevice::destroyRenderPassImpl(RenderPassHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;

    MetalRenderPass* pass = renderPassAllocator_.Get(handle);
    if (pass) {
        renderPassAllocator_.Free(handle);
    }
}


} // namespace primal::graphics::rhi
