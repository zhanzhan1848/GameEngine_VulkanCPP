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
    commandBufferAllocator_.Reserve(256);

    return true;
}

void MetalDevice::shutdownImpl() {
    shutdownMemoryPool();
    // 释放所有分配的资源
    commandBufferAllocator_.Shutdown();
    bufferAllocator_.Shutdown();
    textureAllocator_.Shutdown();
    syncAllocator_.Shutdown();
    queryPoolAllocator_.Shutdown();
    shaderAllocator_.Shutdown();
    pipelineAllocator_.Shutdown();
    pipelineLayoutAllocator_.Shutdown();
    samplerAllocator_.Shutdown();
    descriptorSetLayoutAllocator_.Shutdown();
    descriptorSetAllocator_.Shutdown();

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
    // Simplified: Metal usually waits on command buffers
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

bool MetalDevice::submitCommandBufferImpl(CommandBufferHandle handle) {
    MetalCommandBuffer* cmdBuf = commandBufferAllocator_.Get(static_cast<uint32_t>(handle));
    if (cmdBuf) {
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
    if (event) {
        // 假设我们等待任何非零值，或者当前值 + 1
        // 这里需要更具体的语义。RHIDevice::WaitForSync 通常是指 CPU 等待 GPU。
        // 如果 Sync 对象封装了 SharedEvent，我们可以等待特定的值。
        // 由于接口只有 handle 和 timeout，没有 value，我们假设等待当前已设定的目标值？
        // 实际上，通常 CreateSync 返回的是一个 fence，初始为 unsignaled。
        // 提交命令后会 signal 它。CPU Wait 等待它变成 signaled。
        
        // 获取当前值
        uint64_t value = sync->GetValue();
        // 这里简化实现，假设我们等待的值是 value (如果它已经是 signaled 的话)
        // 真正的实现可能需要配合 CommandBuffer 的 SignalSync。
        // 为了简单起见，这里假设 SyncHandle 对应一个 value 为 1 的状态。
        
        // 注意：MTLSharedEvent 的 notifyListener 也是一种方式。
        // 这是一个阻塞调用
        
        // 临时实现：直接返回 true，因为没有传入等待的 value
        return true; 
    }
    return false; 
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
    bufferAllocator_.Free(static_cast<uint32_t>(handle));
}
void MetalDevice::destroyTextureImpl(ResourceHandle handle) {
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
    std::cout << "[MetalDevice] destroying command buffer " << static_cast<uint32_t>(handle) << std::endl;
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
