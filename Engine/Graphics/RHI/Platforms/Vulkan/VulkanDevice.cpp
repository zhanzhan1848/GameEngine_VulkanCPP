/**
 * @file VulkanDevice.cpp
 * @brief VulkanDevice 实现 — Phase 1: instance + physical + logical device + queues + debug
 * @details 后续 Phase 在此基础上加 VMA、allocators、资源生命周期。
 */

#include "VulkanDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "Engine/Common/CommonHeaders.h"

#include "Graphics/RHI/Core/RHICommand.h"
#include "VulkanTexture.h"
#include "VulkanCommandBuffer.h"
// Phase 4
#include "VulkanShader.h"
#include "VulkanSampler.h"
#include "VulkanDescriptorSetLayout.h"
#include "VulkanPipelineLayout.h"
#include "VulkanDescriptorSet.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanSwapChain.h"

// === VMA: single TU defines implementation ===
#define VMA_IMPLEMENTATION 1
#define VMA_STATIC_VULKAN_FUNCTIONS 0   // 我们已链接 Vulkan::Vulkan,提供 vkGetInstanceProcAddr
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1  // VMA 自己用 ProcAddr 加载其余 vk* 函数
#include <vk_mem_alloc.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <set>

#ifndef VK_NO_PROTOTYPES
// VulkanDevice.cpp links via Vulkan::Vulkan (system loader); prototypes come from header.
#endif

namespace primal::graphics::rhi {

// ============================================================================
// 构造 / 析构
// ============================================================================

VulkanDevice::VulkanDevice(const DeviceDesc& desc)
    : RHIDevice(desc) {
}

VulkanDevice::~VulkanDevice() {
    Shutdown();
}

// ============================================================================
// Phase 1 CRTP 实现：initializeImpl / shutdownImpl / waitIdleImpl
// ============================================================================

bool VulkanDevice::initializeImpl() {
    validationEnabled_ = desc_.enableValidation || desc_.enableDebug;

    if (!createInstance()) {
        std::cerr << "[VulkanDevice] createInstance failed" << std::endl;
        return false;
    }

    if (validationEnabled_ && !setupDebugMessenger()) {
        std::cerr << "[VulkanDevice] Warning: debug messenger setup failed (non-fatal)" << std::endl;
    }

    if (!pickPhysicalDevice()) {
        std::cerr << "[VulkanDevice] pickPhysicalDevice failed" << std::endl;
        return false;
    }

    if (!createLogicalDevice()) {
        std::cerr << "[VulkanDevice] createLogicalDevice failed" << std::endl;
        return false;
    }

    // === Phase 2: VMA allocator ===
    if (!createVmaAllocator()) {
        std::cerr << "[VulkanDevice] createVmaAllocator failed" << std::endl;
        return false;
    }

    // Per-type free_list 预留(Metal 用 1024/512 等;这里先开较小值,后续 Phase 调)
    bufferAllocator_.Reserve(256);
    syncAllocator_.Reserve(64);
    queryPoolAllocator_.Reserve(32);

    // Phase 3: Texture / CommandBuffer — 用 unique_ptr 持有避免不完整类型
    textureAllocator_        = std::make_unique<RHIAllocator<VulkanTexture>>();
    commandBufferAllocator_  = std::make_unique<RHIAllocator<VulkanCommandBuffer>>();
    textureAllocator_->Reserve(128);
    commandBufferAllocator_->Reserve(32);

    // Phase 4: Shader/Sampler/DescSet/Pipeline/RenderPass allocators
    shaderAllocator_                = std::make_unique<RHIAllocator<VulkanShader>>();
    samplerAllocator_               = std::make_unique<RHIAllocator<VulkanSampler>>();
    descriptorSetLayoutAllocator_   = std::make_unique<RHIAllocator<VulkanDescriptorSetLayout>>();
    pipelineLayoutAllocator_        = std::make_unique<RHIAllocator<VulkanPipelineLayout>>();
    descriptorSetAllocator_         = std::make_unique<RHIAllocator<VulkanDescriptorSet>>();
    pipelineAllocator_              = std::make_unique<RHIAllocator<VulkanPipeline>>();
    renderPassAllocator_            = std::make_unique<RHIAllocator<VulkanRenderPass>>();
    shaderAllocator_->Reserve(256);
    samplerAllocator_->Reserve(64);
    descriptorSetLayoutAllocator_->Reserve(128);
    pipelineLayoutAllocator_->Reserve(128);
    descriptorSetAllocator_->Reserve(1024);
    pipelineAllocator_->Reserve(256);
    renderPassAllocator_->Reserve(64);

    // Staging allocator — 4 frame ring of 16MB HOST_VISIBLE pool
    stagingAllocator_.Initialize(device_, vmaAllocator_);

    if (validationEnabled_) {
        std::cout << "[VulkanDevice] Initialized with validation layers enabled." << std::endl;
    } else {
        std::cout << "[VulkanDevice] Initialized (validation disabled)." << std::endl;
    }
    return true;
}

void VulkanDevice::shutdownImpl() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    // 1) GC 双趟:第一趟把所有延迟销毁的 lambda 执行;allocators 释放时可能再入队
    gc_.Shutdown();

    // 2) 释放 per-type allocators(VulkanBuffer 析构会再次 DeferredDestroy)
    bufferAllocator_.Shutdown();
    syncAllocator_.Shutdown();
    queryPoolAllocator_.Shutdown();
    if (commandBufferAllocator_) commandBufferAllocator_->Shutdown();
    if (textureAllocator_)       textureAllocator_->Shutdown();
    // Phase 4
    if (renderPassAllocator_)          renderPassAllocator_->Shutdown();
    if (pipelineAllocator_)            pipelineAllocator_->Shutdown();
    if (descriptorSetAllocator_)       descriptorSetAllocator_->Shutdown();
    if (pipelineLayoutAllocator_)      pipelineLayoutAllocator_->Shutdown();
    if (descriptorSetLayoutAllocator_) descriptorSetLayoutAllocator_->Shutdown();
    if (samplerAllocator_)             samplerAllocator_->Shutdown();
    if (shaderAllocator_)              shaderAllocator_->Shutdown();

    // 3) GC 第二趟:跑上面 allocators 释放时入队的 lambda
    gc_.Shutdown();

    // 4) Staging allocator 持有 VkBuffer + VmaAllocation,必须在 VMA 销毁前释放
    stagingAllocator_.Shutdown();

    // 5) VMA allocator
    destroyVmaAllocator();

    // 6) Native device / instance
    if (device_ != VK_NULL_HANDLE) {
        destroyLogicalDevice();
    }
    if (debugMessenger_ != VK_NULL_HANDLE) {
        teardownDebugMessenger();
    }
    if (instance_ != VK_NULL_HANDLE) {
        destroyInstance();
    }
}

void VulkanDevice::waitIdleImpl() const {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

void VulkanDevice::beginFrameImpl() {
    currentFrameIndex_.store((currentFrameIndex_.load() + 1) % desc_.maxFramesInFlight);
    // 每帧 BeginFrame 把当前 frame 的 staging pool bump offset 归零
    // (上一帧的 blit 应已 submit 完成,frame N+MAX_FRAMES_IN_FLIGHT 的 pool 安全复用)
    stagingAllocator_.BeginFrame();
}

void VulkanDevice::endFrameImpl() {
    // Phase 2+ 在这里 flush staging allocator / per-frame command pool 回收。
}

void VulkanDevice::presentImpl() {
    // Phase 4 实现真正的 present（需要 SwapChain）。
}

void VulkanDevice::queryDeviceInfo(DeviceInfo& info) {
    info.platform = RHIPlatform::Vulkan;

    if (physicalDevice_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        std::strncpy(info.deviceName, props.deviceName, sizeof(info.deviceName) - 1);
        info.deviceName[sizeof(info.deviceName) - 1] = '\0';

        snprintf(info.driverVersion, sizeof(info.driverVersion),
                 "%u.%u.%u",
                 VK_API_VERSION_MAJOR(props.driverVersion),
                 VK_API_VERSION_MINOR(props.driverVersion),
                 VK_API_VERSION_PATCH(props.driverVersion));

        info.maxTexture1DSize = props.limits.maxImageDimension1D;
        info.maxTexture2DSize = props.limits.maxImageDimension2D;
        info.maxTexture3DSize = props.limits.maxImageDimension3D;
        info.maxTextureCubeSize = props.limits.maxImageDimensionCube;
        info.maxRenderTargets = props.limits.maxColorAttachments;
        info.maxVertexAttributes = props.limits.maxVertexInputAttributes;
        info.maxSamplerStates = props.limits.maxPerStageDescriptorSamplers;
        info.maxConstantBufferSize = props.limits.maxUniformBufferRange;
        timestampPeriodNs_ = props.limits.timestampPeriod;
    }

    if (physicalDevice_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
        for (u32 i = 0; i < memProps.memoryHeapCount; ++i) {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                info.dedicatedVideoMemory += memProps.memoryHeaps[i].size;
            } else {
                info.sharedSystemMemory += memProps.memoryHeaps[i].size;
            }
        }
    }

    // Phase 1 不查询 raytracing / mesh shader / VRS；后续 Phase 在此扩展。
    info.supportsRayTracing = false;
    info.supportsMeshShaders = false;
    info.supportsVariableRateShading = false;
}

u32 VulkanDevice::getCurrentFrameIndexImpl() const {
    return currentFrameIndex_.load();
}

// ============================================================================
// Phase 2 实现:Sync / Query / Buffer + VMA
// ============================================================================

SyncHandle VulkanDevice::createSyncImpl() {
    // signaled=false:fence 初始 unsignaled,submit 后才 ping
    u32 id = syncAllocator_.Allocate(device_, false);
    return SyncHandle(id);
}

bool VulkanDevice::waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
    VulkanSync* sync = GetSync(handle);
    if (!sync) return false;
    // VulkanSync::WaitFence 内部已处理 signaled_ 短路
    u64 timeoutNs = static_cast<u64>(timeoutMs) * 1000000ULL;
    return sync->WaitFence(timeoutNs);
}

void VulkanDevice::destroySyncImpl(SyncHandle handle) {
    if (handle == handles::INVALID_SYNC) return;
    VulkanSync* sync = GetSync(handle);
    if (sync) {
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore sem = VK_NULL_HANDLE;
        sync->DetachNatives(&fence, &sem);
        if (fence != VK_NULL_HANDLE || sem != VK_NULL_HANDLE) {
            VkDevice dev = device_;
            gc_.DeferredDestroy([dev, fence, sem]() {
                if (dev != VK_NULL_HANDLE) {
                    if (fence != VK_NULL_HANDLE) vkDestroyFence(dev, fence, nullptr);
                    if (sem  != VK_NULL_HANDLE) vkDestroySemaphore(dev, sem, nullptr);
                }
            });
        }
    }
    syncAllocator_.Free(static_cast<u32>(handle));
}

QueryPoolHandle VulkanDevice::createQueryPoolImpl(const QueryPoolDesc& desc) {
    if (desc.queryCount == 0) return handles::INVALID_QUERY_POOL;
    u32 id = queryPoolAllocator_.Allocate(device_, desc.type, desc.queryCount);
    VulkanQueryPool* pool = queryPoolAllocator_.Get(id);
    if (!pool || pool->GetNativePool() == VK_NULL_HANDLE) {
        std::cerr << "[VulkanDevice] VulkanQueryPool creation failed" << std::endl;
        queryPoolAllocator_.Free(id);
        return handles::INVALID_QUERY_POOL;
    }
    return QueryPoolHandle(id);
}

void VulkanDevice::destroyQueryPoolImpl(QueryPoolHandle handle) {
    if (handle == handles::INVALID_QUERY_POOL) return;
    // T4.6.5 part 24.2 (B9 fix): synchronous destroy. Previous DeferredDestroy
    // lambda raced with ~VulkanQueryPool's own vkDestroyQueryPool call (no
    // DetachNative method exists), causing "Couldn't find VkQueryPool" shutdown
    // errors + GPU lost. Query pools aren't created per-frame, so deferred
    // destruction adds no value — keep it simple.
    queryPoolAllocator_.Free(static_cast<u32>(handle));
}

void VulkanDevice::ResetQueryPool(QueryPoolHandle handle, u32 firstQuery, u32 queryCount) {
    VulkanQueryPool* pool = GetQueryPool(handle);
    if (pool) pool->Reset(firstQuery, queryCount);
}

bool VulkanDevice::getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery,
                                           u32 queryCount, void* data, size_t stride) {
    VulkanQueryPool* pool = GetQueryPool(handle);
    if (!pool) return false;
    return pool->GetResults(firstQuery, queryCount, data, stride);
}

ResourceHandle VulkanDevice::createBufferImpl(const BufferDesc& desc) {
    if (desc.size == 0) {
        std::cerr << "[VulkanDevice] createBufferImpl: size=0 rejected" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    u32 id = bufferAllocator_.Allocate(*this, desc);
    VulkanBuffer* buffer = bufferAllocator_.Get(id);
    if (!buffer) {
        std::cerr << "[VulkanDevice] VulkanBuffer free_list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    if (!buffer->Initialize()) {
        std::cerr << "[VulkanDevice] VulkanBuffer::Initialize failed for id=" << id << std::endl;
        bufferAllocator_.Free(id);
        return handles::INVALID_RESOURCE;
    }
    buffer->SetHandle(ResourceHandle(id));
    // 不调 ResourceManager::Instance().RegisterResource — 跨 dylib ODR 单例分裂,
    // 改用 device-local GetBuffer() 查表。UpdateBufferData override 走这条路。
    return ResourceHandle(id);
}

void VulkanDevice::destroyBufferImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanBuffer* buffer = GetBuffer(handle);
    if (!buffer) {
        std::cerr << "[VulkanDevice] Double-free buffer handle=" << static_cast<u32>(handle)
                  << " — skipping" << std::endl;
        return;
    }
    // VulkanBuffer::~VulkanBuffer 会调 destroyImpl 注册 GC deferred vmaDestroyBuffer。
    // 我们直接 free,析构在 free_list remove 时触发(立即 enqueued 到 GC)。
    bufferAllocator_.Free(static_cast<u32>(handle));
}

void* VulkanDevice::mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) {
    VulkanBuffer* buffer = GetBuffer(handle);
    if (!buffer) return nullptr;
    // VulkanBuffer::mapImpl 已处理 persistent-map 零成本路径 + slow vmaMapMemory 路径
    return buffer->mapImpl(offset, size);
}

void VulkanDevice::unmapBufferImpl(ResourceHandle handle) {
    VulkanBuffer* buffer = GetBuffer(handle);
    if (!buffer) return;
    buffer->unmapImpl();
}

void VulkanDevice::setBufferDirtySizeImpl(ResourceHandle /*handle*/, u64 /*size*/) {
    // Metal 也无 op。Vulkan 走 VMA persistent-map,无需通知 dirty range。
    // 保留接口供 RHI 通用代码调用。
}

bool VulkanDevice::UpdateBufferData(ResourceHandle handle, const void* data, u64 size, u64 offset) {
    // ODR-bypass:跨 dylib 边界时 ResourceManager 单例分裂,GetResource 返回 stale。
    // 必须走 device-local GetBuffer() 而不是 ResourceManager::Instance().GetResource(h)
    VulkanBuffer* buffer = GetBuffer(handle);
    if (!buffer || !data || size == 0) return false;
    return buffer->updateDataImpl(data, size, offset);
}

// ============================================================================
// Phase 5: Shader 热重载
// ============================================================================

void VulkanDevice::RegisterPipelineDependency(PipelineHandle pipeline, ShaderHandle shader) {
    if (pipeline == handles::INVALID_PIPELINE || shader == handles::INVALID_SHADER) return;
    std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
    auto& vec = shaderToPipelines_[shader];
    // 去重
    for (PipelineHandle h : vec) {
        if (h == pipeline) return;
    }
    vec.push_back(pipeline);
}

void VulkanDevice::UnregisterPipelineDependency(PipelineHandle pipeline) {
    std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
    for (auto& pair : shaderToPipelines_) {
        auto& vec = pair.second;
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i] == pipeline) {
                vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
}

bool VulkanDevice::ReloadShader(ShaderHandle shaderHandle, const void* data, size_t size) {
    VulkanShader* shader = GetShader(shaderHandle);
    if (!shader) return false;

    // 1) 重新上传 SPIR-V 字节码 → 新 VkShaderModule
    if (!shader->Reload(data, size)) {
        std::cerr << "[VulkanDevice] Failed to reload shader " << shaderHandle << std::endl;
        return false;
    }

    // 2) Recreate 所有依赖此 shader 的 pipeline
    std::lock_guard<std::mutex> lock(pipelineDependencyMutex_);
    auto it = shaderToPipelines_.find(shaderHandle);
    if (it != shaderToPipelines_.end()) {
        for (PipelineHandle pipelineHandle : it->second) {
            VulkanPipeline* pipeline = GetPipeline(pipelineHandle);
            if (pipeline) {
                if (!pipeline->Recreate()) {
                    std::cerr << "[VulkanDevice] Failed to recreate pipeline " << pipelineHandle
                              << " during shader reload" << std::endl;
                } else {
                    std::cout << "[VulkanDevice] Recreated pipeline " << pipelineHandle
                              << " after shader " << shaderHandle << " reload" << std::endl;
                }
            }
        }
    }
    return true;
}

// ============================================================================
// Phase 3 实现:Texture + CommandBuffer 资源管理
// ============================================================================

VulkanTexture* VulkanDevice::GetTexture(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE || !textureAllocator_) return nullptr;
    return textureAllocator_->Get(static_cast<u32>(handle));
}

VulkanCommandBuffer* VulkanDevice::GetCommandBuffer(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER || !commandBufferAllocator_) return nullptr;
    return commandBufferAllocator_->Get(static_cast<u32>(handle));
}

ResourceHandle VulkanDevice::createTextureImpl(const TextureDesc& desc) {
    if (desc.size.x == 0 || desc.size.y == 0 || desc.format == DataFormat::Unknown) {
        std::cerr << "[VulkanDevice] createTextureImpl: invalid desc (size or format)" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    u32 id = textureAllocator_->Allocate(*this, desc);
    VulkanTexture* tex = textureAllocator_->Get(id);
    if (!tex) {
        std::cerr << "[VulkanDevice] VulkanTexture free_list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    if (!tex->Initialize()) {
        std::cerr << "[VulkanDevice] VulkanTexture::Initialize failed for id=" << id << std::endl;
        textureAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    tex->SetHandle(ResourceHandle(id));
    return ResourceHandle(id);
}

void VulkanDevice::destroyTextureImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanTexture* tex = GetTexture(handle);
    if (!tex) {
        std::cerr << "[VulkanDevice] Double-free texture handle=" << static_cast<u32>(handle)
                  << " — skipping" << std::endl;
        return;
    }
    // VulkanTexture::~VulkanTexture 注册 GC deferred vmaDestroyImage + vkDestroyImageView。
    textureAllocator_->Free(static_cast<u32>(handle));
}

CommandBufferHandle VulkanDevice::createCommandBufferImpl(CommandQueueType type) {
    u32 id = commandBufferAllocator_->Allocate(*this, type);
    VulkanCommandBuffer* cmd = commandBufferAllocator_->Get(id);
    if (!cmd) {
        std::cerr << "[VulkanDevice] VulkanCommandBuffer free_list allocation failed" << std::endl;
        return handles::INVALID_COMMAND_BUFFER;
    }
    if (!cmd->Initialize()) {
        std::cerr << "[VulkanDevice] VulkanCommandBuffer::Initialize failed for id=" << id << std::endl;
        commandBufferAllocator_->Free(id);
        return handles::INVALID_COMMAND_BUFFER;
    }
    cmd->SetHandle(CommandBufferHandle(id));
    // Mirror Metal: register with global CommandBufferManager so production
    // code paths using rhi::GetCommandBuffer() (e.g. IBLPrecomputer) work
    // uniformly across backends.
    RegisterCommandBuffer(cmd);
    return CommandBufferHandle(id);
}

void VulkanDevice::destroyCommandBufferImpl(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER) return;
    VulkanCommandBuffer* cmd = GetCommandBuffer(handle);
    if (!cmd) {
        std::cerr << "[VulkanDevice] Double-free command buffer handle=" << static_cast<u32>(handle)
                  << " — skipping" << std::endl;
        return;
    }
    // T4.6.5 part 30.3 (fence-in-use fix): if the cmd buffer is still in
    // Submitted state, its internal fence is pending on the GPU. Destroying
    // now fires VUID-vkDestroyFence-fence-01120 + VUID-vkFreeCommandBuffers-
    // pCommandBuffers-00047. StandardRenderPipeline's Render() and the windowed
    // test both destroy per-frame cmd buffers immediately after Submit — they
    // rely on the device layer to handle sync. Wait for the fence before
    // freeing; WaitForCompletion is a no-op if state_ != Submitted.
    if (cmd->GetState() == rhi::CommandBufferState::Submitted) {
        cmd->WaitForCompletion();
    }
    UnregisterCommandBuffer(handle);
    // VulkanCommandBuffer::~VulkanCommandBuffer 同步 vkDestroyFence/Pool — 因为命令缓冲内部
    // submit fence 由本对象独占,submit 后 WaitForCompletion 才能销毁(无 deferred 需要)。
    commandBufferAllocator_->Free(static_cast<u32>(handle));
}

// ============================================================================
// Phase 4: Shader / Sampler / Descriptor / Pipeline / RenderPass create/destroy
// ============================================================================

bool VulkanDevice::submitImpl(const QueueSubmitInfo& info) {
    if (info.cmdBuffer == handles::INVALID_COMMAND_BUFFER) return false;

    VulkanCommandBuffer* cmd = GetCommandBuffer(info.cmdBuffer);
    if (!cmd || cmd->GetNativeCmdBuffer() == VK_NULL_HANDLE) return false;

    VkSemaphore waitSem = VK_NULL_HANDLE;
    VkSemaphore signalSem = VK_NULL_HANDLE;
    VkFence signalFence = VK_NULL_HANDLE;

    if (info.waitSemaphore != handles::INVALID_SYNC) {
        if (auto* s = GetSync(info.waitSemaphore)) waitSem = s->GetSemaphore();
    }
    if (info.signalSemaphore != handles::INVALID_SYNC) {
        if (auto* s = GetSync(info.signalSemaphore)) signalSem = s->GetSemaphore();
    }
    if (info.signalFence != handles::INVALID_SYNC) {
        if (auto* s = GetSync(info.signalFence)) {
            signalFence = s->GetFence();
            s->ResetFence();  // 提交前 reset,后续可 WaitForFences
        }
    }

    // T4.6.5 part 24.6 (B8 fix): fall back to the cmd buffer's internal fence
    // when the caller didn't provide one. Without a signaled fence,
    // WaitForCompletion can't synchronize and DestroyCommandBuffer triggers
    // VUID-vkFreeCommandBuffers-pCommandBuffers-00047 (cmd still pending).
    // Always reset before submit — vkQueueSubmit requires unsignaled fences.
    if (signalFence == VK_NULL_HANDLE) {
        signalFence = cmd->GetSubmitFence();
        if (signalFence != VK_NULL_HANDLE) {
            vkResetFences(device_, 1, &signalFence);
        }
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = nullptr;
    si.waitSemaphoreCount = waitSem != VK_NULL_HANDLE ? 1 : 0;
    si.pWaitSemaphores = &waitSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = cmd->GetNativeCmdBuffer();
    si.pCommandBuffers = &cmdBuf;
    si.signalSemaphoreCount = signalSem != VK_NULL_HANDLE ? 1 : 0;
    si.pSignalSemaphores = &signalSem;

    VkResult res = vkQueueSubmit(graphicsQueue_, 1, &si, signalFence);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanDevice] submitImpl vkQueueSubmit failed: " << res << std::endl;
        return false;
    }
    // T4.6.5 part 24.7 (B8 root cause): mark cmd buffer state as Submitted so
    // the base class WaitForCompletion() gate (RHICommand.h:362) lets
    // waitForCompletionImpl actually fire. Without this, state_ stays at
    // RecordingEnded; WaitForCompletion returns false without calling
    // vkWaitForFences; the fence stays pending; DestroyCommandBuffer triggers
    // VUID-vkFreeCommandBuffers-pCommandBuffers-00047 + VUID-vkDestroyFence-fence-01120.
    cmd->SetState(rhi::CommandBufferState::Submitted);
    return true;
}

RHISwapChain* VulkanDevice::createSwapChainImpl(const SwapChainDesc& desc) {
    auto* sc = new VulkanSwapChain(*this, desc);
    if (!sc->Initialize()) {
        std::cerr << "[VulkanDevice] VulkanSwapChain::Initialize failed" << std::endl;
        delete sc;
        return nullptr;
    }
    return sc;
}

void VulkanDevice::destroySwapChainImpl(RHISwapChain* swapChain) {
    if (!swapChain) return;
    vkDeviceWaitIdle(device_);
    delete static_cast<VulkanSwapChain*>(swapChain);
}

ResourceHandle VulkanDevice::createTextureViewImpl(const TextureViewDesc& desc) {
    if (desc.texture == handles::INVALID_RESOURCE || !textureAllocator_) {
        std::cerr << "[VulkanDevice] createTextureViewImpl: invalid source texture handle" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    auto* srcTex = textureAllocator_->Get(static_cast<u32>(desc.texture));
    if (!srcTex || srcTex->GetNativeImage() == VK_NULL_HANDLE) {
        std::cerr << "[VulkanDevice] createTextureViewImpl: source texture not found or null image" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    // 分配新 VulkanTexture slot,view 模式构造(alias src image,自创受限 VkImageView)。
    u32 id = textureAllocator_->Allocate(*this, desc, *srcTex);
    VulkanTexture* viewTex = textureAllocator_->Get(id);
    if (!viewTex) {
        std::cerr << "[VulkanDevice] Texture view free-list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    if (!viewTex->Initialize()) {
        std::cerr << "[VulkanDevice] VulkanTexture::Initialize (view mode) failed for id=" << id << std::endl;
        textureAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    viewTex->SetHandle(ResourceHandle(id));
    return ResourceHandle(id);
}

VulkanShader* VulkanDevice::GetShader(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER || !shaderAllocator_) return nullptr;
    return shaderAllocator_->Get(static_cast<u32>(handle));
}

ShaderHandle VulkanDevice::createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
    if ((!data && size > 0) || stage == ShaderStage::Unknown) {
        std::cerr << "[VulkanDevice] createShaderImpl: invalid args" << std::endl;
        return handles::INVALID_SHADER;
    }
    u32 id = shaderAllocator_->Allocate(*this, data, size, stage, entryPoint);
    VulkanShader* sh = shaderAllocator_->Get(id);
    if (!sh || !sh->Initialize()) {
        if (sh) sh->Destroy();
        shaderAllocator_->Free(id);
        return handles::INVALID_SHADER;
    }
    sh->SetHandle(ShaderHandle(id));
    return ShaderHandle(id);
}

void VulkanDevice::destroyShaderImpl(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER) return;
    VulkanShader* sh = GetShader(handle);
    if (sh) sh->Destroy();
    shaderAllocator_->Free(static_cast<u32>(handle));
}

VulkanSampler* VulkanDevice::GetSampler(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER || !samplerAllocator_) return nullptr;
    return samplerAllocator_->Get(static_cast<u32>(handle));
}

SamplerHandle VulkanDevice::createSamplerImpl(const SamplerDesc& desc) {
    u32 id = samplerAllocator_->Allocate(*this);
    VulkanSampler* s = samplerAllocator_->Get(id);
    if (!s || !s->Initialize(desc)) {
        if (s) s->Destroy();
        samplerAllocator_->Free(id);
        return handles::INVALID_SAMPLER;
    }
    s->SetHandle(SamplerHandle(id));
    return SamplerHandle(id);
}

void VulkanDevice::destroySamplerImpl(SamplerHandle handle) {
    if (handle == handles::INVALID_SAMPLER) return;
    VulkanSampler* s = GetSampler(handle);
    if (s) s->Destroy();
    samplerAllocator_->Free(static_cast<u32>(handle));
}

VulkanDescriptorSetLayout* VulkanDevice::GetDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == handles::INVALID_RESOURCE || !descriptorSetLayoutAllocator_) return nullptr;
    return descriptorSetLayoutAllocator_->Get(static_cast<u32>(handle));
}

DescriptorSetLayoutHandle VulkanDevice::createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) {
    if (!desc.bindings && desc.bindingCount > 0) return handles::INVALID_RESOURCE;
    u32 id = descriptorSetLayoutAllocator_->Allocate(*this, desc);
    VulkanDescriptorSetLayout* l = descriptorSetLayoutAllocator_->Get(id);
    if (!l || !l->Initialize()) {
        if (l) l->destroyImpl();
        descriptorSetLayoutAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    l->SetHandle(ResourceHandle(id));
    return DescriptorSetLayoutHandle(id);
}

void VulkanDevice::destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanDescriptorSetLayout* l = GetDescriptorSetLayout(handle);
    if (l) l->destroyImpl();
    descriptorSetLayoutAllocator_->Free(static_cast<u32>(handle));
}

VulkanPipelineLayout* VulkanDevice::GetPipelineLayout(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT || !pipelineLayoutAllocator_) return nullptr;
    return pipelineLayoutAllocator_->Get(static_cast<u32>(handle));
}

PipelineLayoutHandle VulkanDevice::createPipelineLayoutImpl(const PipelineLayoutDesc& desc) {
    u32 id = pipelineLayoutAllocator_->Allocate(*this, desc);
    VulkanPipelineLayout* l = pipelineLayoutAllocator_->Get(id);
    if (!l || !l->Initialize()) {
        if (l) l->destroyImpl();
        pipelineLayoutAllocator_->Free(id);
        return handles::INVALID_PIPELINE_LAYOUT;
    }
    l->SetHandle(ResourceHandle(id));
    return PipelineLayoutHandle(id);
}

void VulkanDevice::destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {
    if (handle == handles::INVALID_PIPELINE_LAYOUT) return;
    VulkanPipelineLayout* l = GetPipelineLayout(handle);
    if (l) l->destroyImpl();
    pipelineLayoutAllocator_->Free(static_cast<u32>(handle));
}

VulkanDescriptorSet* VulkanDevice::GetDescriptorSet(DescriptorSetHandle handle) {
    if (handle == handles::INVALID_RESOURCE || !descriptorSetAllocator_) return nullptr;
    return descriptorSetAllocator_->Get(static_cast<u32>(handle));
}

DescriptorSetHandle VulkanDevice::createDescriptorSetImpl(const DescriptorSetDesc& desc) {
    if (desc.layout == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;
    u32 id = descriptorSetAllocator_->Allocate(*this, desc);
    VulkanDescriptorSet* s = descriptorSetAllocator_->Get(id);
    if (!s || !s->Initialize()) {
        if (s) s->destroyImpl();
        descriptorSetAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    s->SetHandle(ResourceHandle(id));
    return DescriptorSetHandle(id);
}

void VulkanDevice::destroyDescriptorSetImpl(DescriptorSetHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanDescriptorSet* s = GetDescriptorSet(handle);
    if (s) s->destroyImpl();
    descriptorSetAllocator_->Free(static_cast<u32>(handle));
}

void VulkanDevice::updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes) {
    if (!writes || writeCount == 0) return;
    // 按 dstSet 分组(每个 DescriptorSet::Update 单独处理自己)
    for (u32 i = 0; i < writeCount; ++i) {
        VulkanDescriptorSet* ds = GetDescriptorSet(writes[i].dstSet);
        if (ds) ds->Update(&writes[i], 1);
    }
}

VulkanPipeline* VulkanDevice::GetPipeline(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE || !pipelineAllocator_) return nullptr;
    return pipelineAllocator_->Get(static_cast<u32>(handle));
}

PipelineHandle VulkanDevice::createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
    u32 id = pipelineAllocator_->Allocate(*this);
    VulkanPipeline* p = pipelineAllocator_->Get(id);
    if (!p || !p->Initialize(desc)) {
        if (p) p->Destroy();
        pipelineAllocator_->Free(id);
        return handles::INVALID_PIPELINE;
    }
    p->SetHandle(PipelineHandle(id));
    // Phase 5: 注册 shader→pipeline 依赖,ReloadShader 时 Recreate
    RegisterPipelineDependency(PipelineHandle(id), desc.vertexShader);
    RegisterPipelineDependency(PipelineHandle(id), desc.pixelShader);
    return PipelineHandle(id);
}

PipelineHandle VulkanDevice::createComputePipelineImpl(const ComputePipelineDesc& desc) {
    u32 id = pipelineAllocator_->Allocate(*this);
    VulkanPipeline* p = pipelineAllocator_->Get(id);
    if (!p || !p->Initialize(desc)) {
        if (p) p->Destroy();
        pipelineAllocator_->Free(id);
        return handles::INVALID_PIPELINE;
    }
    p->SetHandle(PipelineHandle(id));
    RegisterPipelineDependency(PipelineHandle(id), desc.computeShader);
    return PipelineHandle(id);
}

void VulkanDevice::destroyPipelineImpl(PipelineHandle handle) {
    if (handle == handles::INVALID_PIPELINE) return;
    VulkanPipeline* p = GetPipeline(handle);
    if (p) p->Destroy();
    UnregisterPipelineDependency(handle);
    pipelineAllocator_->Free(static_cast<u32>(handle));
}

VulkanRenderPass* VulkanDevice::GetRenderPass(RenderPassHandle handle) {
    if (handle == handles::INVALID_RESOURCE || !renderPassAllocator_) return nullptr;
    return renderPassAllocator_->Get(static_cast<u32>(handle));
}

RenderPassHandle VulkanDevice::createRenderPassImpl(const RenderPassDesc& desc) {
    u32 id = renderPassAllocator_->Allocate(*this, desc);
    VulkanRenderPass* rp = renderPassAllocator_->Get(id);
    if (!rp || !rp->Initialize()) {
        if (rp) rp->destroyImpl();
        renderPassAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    rp->SetHandle(ResourceHandle(id));
    return RenderPassHandle(id);
}

void VulkanDevice::destroyRenderPassImpl(RenderPassHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanRenderPass* rp = GetRenderPass(handle);
    if (rp) rp->destroyImpl();
    renderPassAllocator_->Free(static_cast<u32>(handle));
}

// ============================================================================
// VMA allocator 创建 / 销毁
// ============================================================================

bool VulkanDevice::createVmaAllocator() {
    VmaVulkanFunctions vkFuncs{};
    vkFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vkFuncs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.flags                  = 0;  // Phase 5+ 加 BUFFER_DEVICE_ADDRESS / EXT_descriptor_indexing
    ci.instance               = instance_;
    ci.physicalDevice         = physicalDevice_;
    ci.device                 = device_;
    ci.vulkanApiVersion       = VK_API_VERSION_1_3;
    ci.pVulkanFunctions       = &vkFuncs;  // VMA 拷贝到内部状态,栈即可
    ci.pAllocationCallbacks   = nullptr;
    ci.pDeviceMemoryCallbacks = nullptr;
    ci.preferredLargeHeapBlockSize = 0;   // 让 VMA 自选(默认 256MB)

    VkResult res = vmaCreateAllocator(&ci, &vmaAllocator_);
    if (res != VK_SUCCESS) {
        std::cerr << "[VulkanDevice] vmaCreateAllocator failed: " << res << std::endl;
        vmaAllocator_ = nullptr;
        return false;
    }
    return true;
}

void VulkanDevice::destroyVmaAllocator() {
    if (vmaAllocator_) {
        vmaDestroyAllocator(vmaAllocator_);
        vmaAllocator_ = nullptr;
    }
}

// ============================================================================
// VkInstance 创建
// ============================================================================

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    const char* tag = "[Vulkan validation]";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        std::cerr << tag << " ERROR: " << data->pMessage << std::endl;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << tag << " WARN:  " << data->pMessage << std::endl;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        // Info is noisy; route to cout at verbose level only.
    }
    return VK_FALSE;
}

bool supportsLayer(VkInstance instance, const char* layerName) {
    if (instance == VK_NULL_HANDLE) {
        u32 layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        for (const auto& l : layers) {
            if (std::strcmp(l.layerName, layerName) == 0) return true;
        }
        return false;
    }
    return false;
}

} // anonymous namespace

bool VulkanDevice::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "PrimalEngine";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.pEngineName = "Primal";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> enabledLayers;
    if (validationEnabled_) {
        if (supportsLayer(VK_NULL_HANDLE, "VK_LAYER_KHRONOS_validation")) {
            enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
        } else {
            std::cerr << "[VulkanDevice] Validation requested but VK_LAYER_KHRONOS_validation not present" << std::endl;
            validationEnabled_ = false;
        }
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(__APPLE__)
    // Vulkan SDK 1.3+ deprecates VK_MVK_macos_surface in favor of VK_EXT_metal_surface.
    enabledExtensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined(_WIN32)
    enabledExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__linux__)
    enabledExtensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif

    if (validationEnabled_) {
        enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

#if defined(__APPLE__)
    // MoltenVK requires the portability subset flag on physical device selection.
    enabledExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    portabilitySubsetEnabled_ = true;
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = static_cast<u32>(enabledLayers.size());
    createInfo.ppEnabledLayerNames = enabledLayers.data();
    createInfo.enabledExtensionCount = static_cast<u32>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();
#if defined(__APPLE__)
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        std::cerr << "[VulkanDevice] vkCreateInstance failed: " << result << std::endl;
        instance_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VulkanDevice::destroyInstance() {
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::setupDebugMessenger() {
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (!fn) {
        std::cerr << "[VulkanDevice] vkCreateDebugUtilsMessengerEXT not loaded" << std::endl;
        return false;
    }

    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
    ci.pUserData = nullptr;

    if (fn(instance_, &ci, nullptr, &debugMessenger_) != VK_SUCCESS) {
        debugMessenger_ = VK_NULL_HANDLE;
        return false;
    }

    // 顺手加载 vkSetDebugUtilsObjectNameEXT(资源 label,给 VulkanBuffer/VulkanTexture 用)
    vkSetDebugUtilsObjectName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance_, "vkSetDebugUtilsObjectNameEXT"));

    std::cout << "[VulkanDevice] Debug messenger initialized." << std::endl;
    return true;
}

void VulkanDevice::teardownDebugMessenger() {
    if (debugMessenger_ == VK_NULL_HANDLE) return;
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) {
        fn(instance_, debugMessenger_, nullptr);
    }
    debugMessenger_ = VK_NULL_HANDLE;
}

// ============================================================================
// VkPhysicalDevice 选择
// ============================================================================

u32 VulkanDevice::findQueueFamily(VkQueueFlags flags) const {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, families.data());

    // Prefer exact-match (dedicated) first; fall back to flags-superset.
    u32 fallback = UINT32_MAX;
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & flags) == flags) {
            if (families[i].queueFlags == flags) return i;
            if (fallback == UINT32_MAX) fallback = i;
        }
    }
    return fallback;
}

bool VulkanDevice::pickPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        std::cerr << "[VulkanDevice] No Vulkan-capable physical devices" << std::endl;
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Pick first VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, else first integrated, else first any.
    VkPhysicalDevice discrete = VK_NULL_HANDLE;
    VkPhysicalDevice integrated = VK_NULL_HANDLE;
    VkPhysicalDevice any = VK_NULL_HANDLE;
    for (auto d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && discrete == VK_NULL_HANDLE) discrete = d;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && integrated == VK_NULL_HANDLE) integrated = d;
        if (any == VK_NULL_HANDLE) any = d;
    }
    physicalDevice_ = discrete != VK_NULL_HANDLE ? discrete : (integrated != VK_NULL_HANDLE ? integrated : any);
    if (physicalDevice_ == VK_NULL_HANDLE) {
        std::cerr << "[VulkanDevice] No suitable physical device" << std::endl;
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    std::cout << "[VulkanDevice] Picked physical device: " << props.deviceName << std::endl;
    return true;
}

bool VulkanDevice::createLogicalDevice() {
    graphicsQueueFamily_ = findQueueFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    if (graphicsQueueFamily_ == UINT32_MAX) {
        std::cerr << "[VulkanDevice] No graphics/compute queue family found" << std::endl;
        return false;
    }
    // Phase 1: 复用 graphics queue 作为 compute / transfer；Phase 3+ 找专用 async queue。
    computeQueueFamily_ = graphicsQueueFamily_;
    transferQueueFamily_ = graphicsQueueFamily_;

    std::set<u32> uniqueFamilies{graphicsQueueFamily_};
    std::vector<VkDeviceQueueCreateInfo> queueCIs;
    float priority = 1.0f;
    for (u32 fam : uniqueFamilies) {
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = fam;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queueCIs.push_back(q);
    }

    // 查询物理设备实际支持的功能,按需启用。MoltenVK 不支持 geometryShader/tessellationShader
    // 等;不能硬开,否则 vkCreateDevice 返回 VK_ERROR_FEATURE_NOT_PRESENT。
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);

    VkPhysicalDeviceFeatures features{};
    if (supported.samplerAnisotropy) features.samplerAnisotropy = VK_TRUE;
    if (supported.imageCubeArray)    features.imageCubeArray = VK_TRUE;
    if (supported.geometryShader)    features.geometryShader = VK_TRUE;
    if (supported.depthClamp)        features.depthClamp = VK_TRUE;
    if (supported.fillModeNonSolid)  features.fillModeNonSolid = VK_TRUE;
    // Phase 4+ 在 features2 链中开 mesh shader / raytracing / bindless。

    std::vector<const char*> deviceExtensions;
    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#if defined(__APPLE__)
    // MoltenVK portability subset is mandatory on macOS.
    deviceExtensions.push_back("VK_KHR_portability_subset");
#endif

    // Vulkan 1.0+ 弃用 device-level layers;validation 必须在 instance 层开。
    // 保留 enabledLayerCount=0 以满足 VUID-VkDeviceCreateInfo-enabledLayerCount-12384。

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = static_cast<u32>(queueCIs.size());
    ci.pQueueCreateInfos = queueCIs.data();
    ci.enabledLayerCount = 0;
    ci.ppEnabledLayerNames = nullptr;
    ci.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    ci.ppEnabledExtensionNames = deviceExtensions.data();
    ci.pEnabledFeatures = &features;

    if (vkCreateDevice(physicalDevice_, &ci, nullptr, &device_) != VK_SUCCESS) {
        std::cerr << "[VulkanDevice] vkCreateDevice failed" << std::endl;
        device_ = VK_NULL_HANDLE;
        return false;
    }

    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    computeQueue_ = graphicsQueue_;
    transferQueue_ = graphicsQueue_;
    return true;
}

void VulkanDevice::destroyLogicalDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        graphicsQueue_ = computeQueue_ = transferQueue_ = VK_NULL_HANDLE;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
