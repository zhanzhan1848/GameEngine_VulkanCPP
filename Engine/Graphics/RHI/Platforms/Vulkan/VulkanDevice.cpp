/**
 * @file VulkanDevice.cpp
 * @brief VulkanDevice 实现 — Phase 1: instance + physical + logical device + queues + debug
 * @details 后续 Phase 在此基础上加 VMA、allocators、资源生命周期。
 */

#include "VulkanDevice.h"
#include "Engine/Common/CoreLogStream.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Common/CoreDiag.h"
#include "Engine/Graphics/Utilities/CPUStallAudit.h"

#include "Graphics/RHI/Core/RHICommand.h"
#include "VulkanBackendLinks.h"
#include "VulkanTexture.h"
#include "VulkanCommandBuffer.h"
#include "VulkanHangDiag.h"
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
#include <algorithm>
#include <cstring>
#include <set>

#ifndef VK_NO_PROTOTYPES
// VulkanDevice.cpp links via Vulkan::Vulkan (system loader); prototypes come from header.
#endif

namespace primal::graphics::rhi {
namespace {
std::atomic<u64> nextVulkanDeviceGeneration{1};
u64 AllocateVulkanDeviceGeneration() {
    u64 current=nextVulkanDeviceGeneration.load(std::memory_order_relaxed);
    for(;;) {
        if(current==~u64(0))return 0; // fail closed on exhaustion; never reuse an id
        if(nextVulkanDeviceGeneration.compare_exchange_weak(current,current+1,std::memory_order_relaxed))return current;
    }
}
}

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
    lifecycleGeneration_=0;
    lastWaitIdleResult_.store(VK_NOT_READY,std::memory_order_release);
    validationEnabled_ = desc_.enableValidation || desc_.enableDebug;

    // F0-11 链接锚点：强制拉入 capture 桥 / pass timestamp 入口的注册 TU
    //（静态库无符号引用成员会被链接器裁剪，注册曾静默丢失——见
    // VulkanBackendLinks.h）。幂等；不改变设备初始化语义。
    EnsureVulkanCaptureBridgeLinked();
    pass_timestamp::EnsureVulkanPassTimestampEntryLinked();

    if (!createInstance()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createInstance failed" << std::endl;
        return false;
    }

    if (validationEnabled_ && !setupDebugMessenger()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Warning: debug messenger setup failed (non-fatal)" << std::endl;
    }

    if (!pickPhysicalDevice()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] pickPhysicalDevice failed" << std::endl;
        return false;
    }

    if (!createLogicalDevice()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createLogicalDevice failed" << std::endl;
        return false;
    }

    // === Phase 2: VMA allocator ===
    if (!createVmaAllocator()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createVmaAllocator failed" << std::endl;
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
    // P4c-F4:传入 queue + family,FlushBlocking(立即模式)可直接提交
    stagingAllocator_.Initialize(device_, vmaAllocator_, graphicsQueue_, graphicsQueueFamily_);

    // === P4c-F6: SetComputeBytes >128B 隐式 UBO 回退基建 ===
    // 1MB HOST_VISIBLE UBO + UNIFORM_BUFFER_DYNAMIC descriptor(set 3 约定,
    // 见 RHIShaderCommon.glsl)。失败不致命 — 回退路径禁用,走 push 上限。
    {
        constexpr u64 kImplicitUBOBytes = 1ULL << 20;
        VkBufferCreateInfo uboCI{};
        uboCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboCI.size = kImplicitUBOBytes;
        uboCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo uboACI{};
        uboACI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                     | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        uboACI.usage = VMA_MEMORY_USAGE_AUTO;
        VmaAllocationInfo uboInfo{};
        if (vmaCreateBuffer(vmaAllocator_, &uboCI, &uboACI, &implicitComputeUBO_,
                            &implicitComputeUBOAlloc_, &uboInfo) == VK_SUCCESS) {
            implicitComputeUBOMapped_ = uboInfo.pMappedData;
            implicitComputeUBOCapacity_ = kImplicitUBOBytes;

            VkDescriptorSetLayoutBinding b{};
            b.binding = 0;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo dslCI{};
            dslCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dslCI.bindingCount = 1;
            dslCI.pBindings = &b;
            vkCreateDescriptorSetLayout(device_, &dslCI, nullptr, &implicitComputeSetLayout_);

            // set 空档填充用空 DSL(0 binding)
            VkDescriptorSetLayoutCreateInfo emptyCI{};
            emptyCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            emptyCI.bindingCount = 0;
            vkCreateDescriptorSetLayout(device_, &emptyCI, nullptr, &emptyPaddingSetLayout_);

            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            poolSize.descriptorCount = 1;
            VkDescriptorPoolCreateInfo poolCI{};
            poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolCI.maxSets = 1;
            poolCI.poolSizeCount = 1;
            poolCI.pPoolSizes = &poolSize;
            if (vkCreateDescriptorPool(device_, &poolCI, nullptr, &implicitComputePool_) == VK_SUCCESS) {
                VkDescriptorSetAllocateInfo dsAI{};
                dsAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                dsAI.descriptorPool = implicitComputePool_;
                dsAI.descriptorSetCount = 1;
                dsAI.pSetLayouts = &implicitComputeSetLayout_;
                vkAllocateDescriptorSets(device_, &dsAI, &implicitComputeDescSet_);

                VkDescriptorBufferInfo dbi{};
                dbi.buffer = implicitComputeUBO_;
                dbi.offset = 0;
                // range ≤ maxUniformBufferRange（VUID-VkWriteDescriptorSet-
                // descriptorType-00332）：桌面 GPU 常为 64KB，region 容量 256KB
                // 直接作 range 在这些设备上触发验证错误（八测实测 262144 >
                // 65536，MoltenVK 上限更高所以 macOS 从未暴露）。取 min：
                // 上限足够的实现（MoltenVK）行为零变化；上限更小的实现按
                // 设备实际上限收缩——真实单次用量 = SetComputeBytes 的 size
                // （P4c 验收形态 ≈ maxPushConstants+512=640B，远小于 64KB），
                // 超限请求由 AllocImplicitComputeUBO 显式拒绝（同 region 耗尽
                // 的丢弃路径+Error 日志，不静默截断）。动态偏移(region 基址 +
                // bump) + range 仍恰好不超过 buffer 总量(VUID-01979)。
                VkPhysicalDeviceProperties pdProps{};
                vkGetPhysicalDeviceProperties(physicalDevice_, &pdProps);
                implicitComputeUBORange_ =
                    std::min<u64>(kImplicitUBOBytes / 4,
                                  static_cast<u64>(pdProps.limits.maxUniformBufferRange));
                dbi.range = implicitComputeUBORange_;
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = implicitComputeDescSet_;
                w.dstBinding = 0;
                w.dstArrayElement = 0;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w.pBufferInfo = &dbi;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
        } else {
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] implicit compute UBO creation failed — "
                         "SetComputeBytes >maxPushConstants will be dropped" << std::endl;
            implicitComputeUBO_ = VK_NULL_HANDLE;
        }
    }

    if (validationEnabled_) {
        PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Initialized with validation layers enabled." << std::endl;
    } else {
        PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Initialized (validation disabled)." << std::endl;
    }
    // R10 taskC：诊断初始化（memory budget 扩展支持 + 初始余量；门控）。
    hangdiag::NoteInit(physicalDevice_, device_);
    lifecycleGeneration_=AllocateVulkanDeviceGeneration();
    return true;
}

void VulkanDevice::shutdownImpl() {
    lifecycleGeneration_=0;
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    // P4c-F6: 隐式 compute UBO 基建(pool → set → layout → buffer)
    if (device_ != VK_NULL_HANDLE) {
        if (implicitComputePool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, implicitComputePool_, nullptr);
            implicitComputePool_ = VK_NULL_HANDLE;
            implicitComputeDescSet_ = VK_NULL_HANDLE;
        }
        if (implicitComputeSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, implicitComputeSetLayout_, nullptr);
            implicitComputeSetLayout_ = VK_NULL_HANDLE;
        }
        if (emptyPaddingSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, emptyPaddingSetLayout_, nullptr);
            emptyPaddingSetLayout_ = VK_NULL_HANDLE;
        }
        if (implicitComputeUBO_ != VK_NULL_HANDLE && implicitComputeUBOAlloc_) {
            vmaDestroyBuffer(vmaAllocator_, implicitComputeUBO_, implicitComputeUBOAlloc_);
            implicitComputeUBO_ = VK_NULL_HANDLE;
            implicitComputeUBOAlloc_ = nullptr;
            implicitComputeUBOMapped_ = nullptr;
        }
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
    lastWaitIdleResult_=device_!=VK_NULL_HANDLE?vkDeviceWaitIdle(device_):VK_ERROR_DEVICE_LOST;
    // R12-3.2：WaitIdle 非 SUCCESS 无条件落日志（此前仅 hang-diag 门控内可见，
    // r12 Windows 关机日志里 device-lost 只能从下游副作用猜）。行为零变化——
    // 只加一行 Error。
    if (lastWaitIdleResult_.load() != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] WaitIdle returned "
                  << static_cast<int>(lastWaitIdleResult_.load()) << " (device lost / driver error)" << std::endl;
    }
    // R10 taskC：WaitIdle 返回 device lost 时 fail-fast（门控）。
    if (lastWaitIdleResult_.load() == VK_ERROR_DEVICE_LOST)
        hangdiag::ReportDeviceLost("VulkanDevice::WaitIdle", lastWaitIdleResult_.load());
}

void VulkanDevice::beginFrameImpl() {
    currentFrameIndex_.store((currentFrameIndex_.load() + 1) % desc_.maxFramesInFlight);
    // R10 taskC：诊断帧计数（仅 PRIMAL_VK_HANG_DIAG=1 时递增/打印）。
    hangdiag::NoteFrameBegin();
    // 每帧 BeginFrame 把当前 frame 的 staging pool bump offset 归零
    // (上一帧的 blit 应已 submit 完成,frame N+MAX_FRAMES_IN_FLIGHT 的 pool 安全复用)
    stagingAllocator_.BeginFrame();

    // R12-3.4: 纹理 retire 队列的帧时钟无条件推进（RetireTexture 排入的槽位
    // 在 kRetireFreeLag 帧后回收+代际推进）。这与下面的 PRIMAL_DEFERRED_FREE
    // 门无关：Free() 延迟保持默认关（Metal 有已知崩溃不启用），retire 是
    // 独立队列、仅由显式 RetireTexture 使用。
    if (textureAllocator_) {
        textureAllocator_->TickRetires(++textureRetireFrame_);
        // R12-3.4 诊断（默认关）：每 30 帧打印活纹理的 VMA 持有字节总量。
        // MoltenVK 的 heapUsage 不随 vmaDestroyImage 即时回落（VMA 池保留
        // VkDeviceMemory 块），双驻留消除的判据以本计数为准。
        static const bool s_vramDiag = std::getenv("PRIMAL_VK_VRAM_DIAG") != nullptr;
        if (s_vramDiag && textureRetireFrame_ % 30 == 0) {
            u64 ownedBytes = 0, ownedCount = 0, pendingRetires = 0;
            textureAllocator_->ForEach([&](VulkanTexture& tex) {
                if (!tex.OwnsImage() || !tex.GetNativeImage()) return;
                VmaAllocationInfo info{};
                vmaGetAllocationInfo(vmaAllocator_, tex.GetAllocation(), &info);
                if (info.size) { ownedBytes += info.size; ++ownedCount; }
            });
            textureAllocator_->PendingRetireCount(pendingRetires);
            std::fprintf(stderr,
                "[R12-3.4] frame=%llu liveOwnedImages=%llu ownedBytes=%.1fMB pendingRetires=%llu\n",
                static_cast<unsigned long long>(textureRetireFrame_),
                static_cast<unsigned long long>(ownedCount),
                static_cast<double>(ownedBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(pendingRetires));
        }
    }

    // R1/T0.13: 驱动延迟槽位释放 — 已释放槽保留 kDeferredFreeLag 帧，防止
    // in-flight 帧持有的 ResourceHandle 被新分配别名到新对象（C1-H2）。
    // DEFAULT OFF — see MetalDevice::beginFrameImpl for the rationale.
    static const bool deferredFreeEnabled =
        std::getenv("PRIMAL_DEFERRED_FREE") != nullptr;
    if (!deferredFreeEnabled) return;
    const u64 deferredFrame = ++deferred_frame_counter_;
    bufferAllocator_.TickDeferredRelease(deferredFrame);
    syncAllocator_.TickDeferredRelease(deferredFrame);
    queryPoolAllocator_.TickDeferredRelease(deferredFrame);
    if (textureAllocator_)             textureAllocator_->TickDeferredRelease(deferredFrame);
    if (commandBufferAllocator_)       commandBufferAllocator_->TickDeferredRelease(deferredFrame);
    if (shaderAllocator_)              shaderAllocator_->TickDeferredRelease(deferredFrame);
    if (samplerAllocator_)             samplerAllocator_->TickDeferredRelease(deferredFrame);
    if (descriptorSetLayoutAllocator_) descriptorSetLayoutAllocator_->TickDeferredRelease(deferredFrame);
    if (pipelineLayoutAllocator_)      pipelineLayoutAllocator_->TickDeferredRelease(deferredFrame);
    if (descriptorSetAllocator_)       descriptorSetAllocator_->TickDeferredRelease(deferredFrame);
}

VulkanDevice::ImplicitUBOAlloc VulkanDevice::AllocImplicitComputeUBO(u32 size) {
    ImplicitUBOAlloc ret{};
    if (implicitComputeUBOMapped_ == nullptr || size == 0) return ret;
    // VUID-00332 收缩后的 range 上限：单次 payload 超过设备 maxUniformBufferRange
    // 时无法用 uniform descriptor 表达（走 SSBO 是另一形态，超出本回退的
    // 设计面）——与 region 耗尽同路拒绝，调用方打 Error 日志。
    if (implicitComputeUBORange_ == 0 || size > implicitComputeUBORange_) return ret;

    // per-frame-region ring:1MB / 4 region = 每 region 256KB 独立地址段
    // (region 间基址隔离 — 跨帧的 SetComputeBytes 数据不会互相覆写;
    // region 复用安全性同 staging pool:BeginFrame 已等过 frame N-3 fence)
    const u64 regionCapacity = implicitComputeUBOCapacity_ / 4;
    const u32 region = currentFrameIndex_.load() % 4;
    if (region != implicitUBOLastRegion_) {
        implicitUBOLastRegion_ = region;
        implicitUBOOffset_[region] = 0;
    }
    constexpr u64 kAlign = 256;  // UBO 动态偏移的标准对齐(minUboOffsetAlign ≤ 256)
    u64 bump = (implicitUBOOffset_[region] + kAlign - 1) & ~(kAlign - 1);
    if (bump + size > regionCapacity) return ret;  // region 容量耗尽

    implicitUBOOffset_[region] = bump + size;
    const u64 off = u64(region) * regionCapacity + bump;
    ret.dst = static_cast<u8*>(implicitComputeUBOMapped_) + off;
    ret.offset = off;
    ret.valid = true;
    return ret;
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
        // P4c-F6: SetComputeBytes 回退阈值
        maxPushConstantsSize_ = props.limits.maxPushConstantsSize;
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

    // VT M0.8: ASTC tile 格式采样能力(MoltenVK/Apple Silicon 为 true;
    // 桌面离散 GPU 通常 false — 烘焙管线据此选 BCn 或 ASTC 数据段)。
    if (physicalDevice_ != VK_NULL_HANDLE) {
        VkFormatProperties astcProps{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_,
                                            VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astcProps);
        info.supportsASTC = astcProps.optimalTilingFeatures != 0;
    }
}

u32 VulkanDevice::getCurrentFrameIndexImpl() const {
    return currentFrameIndex_.load();
}

// ============================================================================
// Phase 2 实现:Sync / Query / Buffer + VMA
// ============================================================================

SyncHandle VulkanDevice::createSyncImpl() {
    // T4.6.5 part 32: fence 初始 signaled=true。RenderSystem 持有 MAX_FRAMES_IN_FLIGHT 个
    // frameFences_,首帧 Wait 立即返回(避免 ~1s timeout × N slots 的启动卡顿)。
    // submit-then-wait 调用方安全:submitImpl 在 vkQueueSubmit 前调 ResetFence(无论初始态)。
    // 显式需要 unsignaled 语义的测试/调用方应在 CreateSync 后手动 ResetFence。
    u32 id = syncAllocator_.Allocate(device_, true);
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
            hangdiag::RetireFence(fence);
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanQueryPool creation failed" << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createBufferImpl: size=0 rejected" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    u32 id = bufferAllocator_.Allocate(*this, desc);
    VulkanBuffer* buffer = bufferAllocator_.Get(id);
    if (!buffer) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanBuffer free_list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    if (!buffer->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanBuffer::Initialize failed for id=" << id << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Double-free buffer handle=" << static_cast<u32>(handle)
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

// VT M0.7: 子区域上传 — 同 ODR-bypass 理由走 device-local 查表
bool VulkanDevice::UpdateTextureRegion(ResourceHandle handle, const void* data, u64 size,
                                        const TextureRegionUpdate& region) {
    VulkanTexture* texture = GetTexture(handle);
    if (!texture || !data || size == 0) return false;
    return texture->updateRegion(data, size, region);
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Failed to reload shader " << shaderHandle << std::endl;
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
                    PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Failed to recreate pipeline " << pipelineHandle
                              << " during shader reload" << std::endl;
                } else {
                    PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Recreated pipeline " << pipelineHandle
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
    // R12-3.4: 代际校验解析——句柄 = (generation<<32)|slot id。槽位回收复用后
    // generation 已推进，过期句柄解析为 nullptr 而不是槽位新对象（R11 taskC
    // 的 id 别名 hazard：回收 id 上的新 1×1 占位被旧句柄解析成语义别名，
    // 20 条布局错配）。未编码句柄（gen=0）同样不匹配——生产者必须走
    // MakeHandle（createTextureImpl / createTextureViewImpl / SwapChain wrap）。
    return textureAllocator_->GetChecked(handle);
}

bool VulkanDevice::GetBufferByteSize(ResourceHandle buffer, u64& bytes) {
    auto* resource=GetBuffer(buffer);
    bytes=resource ? resource->GetSize() : 0;
    return bytes!=0;
}

bool VulkanDevice::GetTextureSize(ResourceHandle texture, u32& width, u32& height) {
    auto* tex = GetTexture(texture);
    if (!tex || !tex->GetNativeImage()) return false;
    const auto& desc = tex->GetTextureDesc();
    if (!desc.size.x || !desc.size.y) return false;
    const u32 mip = tex->isView_ ? tex->viewDesc_.mostDetailedMip : 0u;
    width = std::max(1u, desc.size.x >> mip);
    height = std::max(1u, desc.size.y >> mip);
    return true;
}

DataFormat VulkanDevice::GetTextureFormat(ResourceHandle texture) {
    auto* tex=GetTexture(texture);
    return tex && tex->GetNativeImage() ? tex->GetTextureDesc().format : DataFormat::Unknown;
}

VulkanCommandBuffer* VulkanDevice::GetCommandBuffer(CommandBufferHandle handle) {
    if (handle == handles::INVALID_COMMAND_BUFFER || !commandBufferAllocator_) return nullptr;
    return commandBufferAllocator_->Get(static_cast<u32>(handle));
}

ResourceHandle VulkanDevice::createTextureImpl(const TextureDesc& desc) {
    if (desc.size.x == 0 || desc.size.y == 0 || desc.format == DataFormat::Unknown) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createTextureImpl: invalid desc (size or format)" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    u32 id = textureAllocator_->Allocate(*this, desc);
    VulkanTexture* tex = textureAllocator_->Get(id);
    if (!tex) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanTexture free_list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    if (!tex->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanTexture::Initialize failed for id=" << id << std::endl;
        textureAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    // R12-3.4: 句柄带代际编码（generation<<32|id），GetTexture 按代际校验。
    const ResourceHandle handle = ResourceHandle(textureAllocator_->MakeHandle(id));
    tex->SetHandle(handle);
    // P0-B 定位（PRIMAL_RG_LAYOUT_TRACE=1）: 创建序号+名字+尺寸，与
    // [DSSAMPLED]/[RGTRACE] 的 VkImage 句柄对照，定位匿名纹理的创建流程。
    if (std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
        fprintf(stderr, "[TEXCREATE] id=%u img=0x%llx name='%s' %ux%u fmt=%d mips=%u\n",
                id, (unsigned long long)(uintptr_t)tex->GetNativeImage(),
                desc.name.c_str(), desc.size.x, desc.size.y,
                (int)desc.format, desc.mipLevels);
    }
    return handle;
}

void VulkanDevice::destroyTextureImpl(ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanTexture* tex = GetTexture(handle);
    if (!tex) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Double-free texture handle=" << static_cast<u32>(handle)
                  << " — skipping" << std::endl;
        return;
    }
    // VulkanTexture::~VulkanTexture 注册 GC deferred vmaDestroyImage + vkDestroyImageView。
    textureAllocator_->Free(static_cast<u32>(handle));
}

void VulkanDevice::RetireTexture(rhi::ResourceHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanTexture* tex = GetTexture(handle);
    if (!tex) {
        // 已释放/已 retire/代际过期：无事可做。区别于 destroyTextureImpl 的
        // 显式销毁路径，这里保持安静（批量释放的幂等护栏，非错误）。
        return;
    }
    // R12-3.4: 槽位入 retire 队列——不立即回池。对象保持存活到
    // TickRetires 推进 kRetireFreeLag 帧后（析构 → GC 延迟销毁原生
    // VkImage/VkView + 槽位代际+1）。R11 taskC 复检证明立即回池会让
    // 紧随其后的批量分配（帧 0-1 GI 重初始化 ~2000 个）拿到回收 id，
    // 已捕获旧句柄值的结构把新对象解析成语义别名（20 条布局错配）。
    textureAllocator_->Retire(static_cast<u32>(handle));
    if (std::getenv("PRIMAL_VK_VRAM_DIAG")) {
        fprintf(stderr, "[R12-3.4] RetireTexture id=%u gen=%u name='%s' (%ux%u fmt=%d)\n",
                static_cast<u32>(handle), static_cast<u32>(handle >> 32),
                tex->GetTextureDesc().name.c_str(),
                tex->GetTextureDesc().size.x, tex->GetTextureDesc().size.y,
                (int)tex->GetTextureDesc().format);
    }
}

CommandBufferHandle VulkanDevice::createCommandBufferImpl(CommandQueueType type) {
    u32 id = commandBufferAllocator_->Allocate(*this, type);
    VulkanCommandBuffer* cmd = commandBufferAllocator_->Get(id);
    if (!cmd) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanCommandBuffer free_list allocation failed" << std::endl;
        return handles::INVALID_COMMAND_BUFFER;
    }
    if (!cmd->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanCommandBuffer::Initialize failed for id=" << id << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Double-free command buffer handle=" << static_cast<u32>(handle)
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
    lastWaitIdleResult_.store(VK_NOT_READY,std::memory_order_release);
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
            { primal::graphics::cpu_stall_audit::Scope cpuScope("vulkan.submit.reset_fence");
                s->ResetFence(); }  // 提交前 reset,后续可 WaitForFences
        }
    }

    // T4.6.5 part 24.6 (B8 fix): fall back to the cmd buffer's internal fence
    // when the caller didn't provide one. Without a signaled fence,
    // WaitForCompletion can't synchronize and DestroyCommandBuffer triggers
    // VUID-vkFreeCommandBuffers-pCommandBuffers-00047 (cmd still pending).
    // Always reset before submit — vkQueueSubmit requires unsignaled fences.
    //
    // T4.6.5 part 30.6 (X7 fix): track whether we used an external fence.
    // If true, the cmd's internal submitFence_ is NEVER signaled by this
    // submit — VulkanCommandBuffer::waitForCompletionImpl must skip its
    // vkWaitForFences call (would 5s-timeout on the unused fence).
    if (signalFence == VK_NULL_HANDLE) {
        signalFence = cmd->GetSubmitFence();
        if (signalFence != VK_NULL_HANDLE) {
            { primal::graphics::cpu_stall_audit::Scope cpuScope("vulkan.submit.reset_fence");
                vkResetFences(device_, 1, &signalFence); }
        }
        cmd->externalFenceSignaled_ = false;
        cmd->externalFence_ = VK_NULL_HANDLE;
    } else {
        cmd->externalFenceSignaled_ = true;
        cmd->externalFence_ = signalFence;  // P0.1: 供 WaitForCompletion 兜底等待
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

    VkResult res;
    {
        primal::graphics::cpu_stall_audit::Scope cpuScope("vulkan.submit.queue_submit");
        // R10 taskC：vkQueueSubmit 在队列 wedged 时可能阻塞在驱动内——
        // 门控看门狗；成功后登记 fence/计数供超时 dump 使用。
        hangdiag::ScopedHangWatch watch("queueSubmit", "submitImpl");
        res = vkQueueSubmit(graphicsQueue_, 1, &si, signalFence);
    }
    if (res != VK_SUCCESS) {
        if (res == VK_ERROR_DEVICE_LOST) hangdiag::ReportDeviceLost("submitImpl", res);
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] submitImpl vkQueueSubmit failed: " << res << std::endl;
        return false;
    }
    hangdiag::NoteSignalFence(signalFence);
    hangdiag::NoteSubmitOk(1);
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanSwapChain::Initialize failed" << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createTextureViewImpl: invalid source texture handle" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    auto* srcTex = textureAllocator_->GetChecked(desc.texture);
    if (!srcTex || srcTex->GetNativeImage() == VK_NULL_HANDLE) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createTextureViewImpl: source texture not found or null image" << std::endl;
        return handles::INVALID_RESOURCE;
    }

    // 分配新 VulkanTexture slot,view 模式构造(alias src image,自创受限 VkImageView)。
    u32 id = textureAllocator_->Allocate(*this, desc, *srcTex);
    VulkanTexture* viewTex = textureAllocator_->Get(id);
    if (!viewTex) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Texture view free-list allocation failed" << std::endl;
        return handles::INVALID_RESOURCE;
    }
    if (!viewTex->Initialize()) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] VulkanTexture::Initialize (view mode) failed for id=" << id << std::endl;
        textureAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    // R12-3.4: 代际编码句柄（与 createTextureImpl 一致）。
    const ResourceHandle handle = ResourceHandle(textureAllocator_->MakeHandle(id));
    viewTex->SetHandle(handle);
    return handle;
}

VulkanShader* VulkanDevice::GetShader(ShaderHandle handle) {
    if (handle == handles::INVALID_SHADER || !shaderAllocator_) return nullptr;
    return shaderAllocator_->Get(static_cast<u32>(handle));
}

ShaderHandle VulkanDevice::createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
    if ((!data && size > 0) || stage == ShaderStage::Unknown) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] createShaderImpl: invalid args" << std::endl;
        return handles::INVALID_SHADER;
    }
    // R7-WIN 诊断(01379):SPIR-V 预检 —— magic 0x07230203(小端字节序
    // 03 02 23 07)+ size 必须是 4 的倍数。本函数只接受 Vulkan 字节码;
    // 喂进 DXBC .cso 或 GLSL/WGSL/Metal 源文本时 vkCreateShaderModule
    // 必定失败(Initialize 返回 false 后本函数同样返回 INVALID_SHADER),
    // 提前拒绝只是省一次无效驱动调用并留下可定位日志(entryPoint+判类+
    // hex)。合法 .spv(Mac/Windows 正常路径)首四字节即 magic,预检恒过、
    // 行为零变化。
    {
        const u8* spirvBytes = static_cast<const u8*>(data);
        const bool spirvHeaderOk = size >= 4 && (size % 4) == 0 &&
                                   spirvBytes[0] == 0x03 && spirvBytes[1] == 0x02 &&
                                   spirvBytes[2] == 0x23 && spirvBytes[3] == 0x07;
        if (!spirvHeaderOk) {
            PRIMAL_LOG_STREAM(Error, "RHI")
                << "[VulkanShader] reject non-SPIR-V bytes: entryPoint="
                << (entryPoint ? entryPoint : "(null)") << " size=" << size << " "
                << detail::DescribeBadSpirvBytes(data, size) << std::endl;
            return handles::INVALID_SHADER;
        }
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
    if (!s) {
        // T4.6.5 part 35.7: mirror destroyTextureImpl's double-free guard.
        // Without this, Free() below hits free_list::remove's "already
        // removed" assertion when a sampler is destroyed twice (e.g., the
        // pipeline destroys texture_sampler_ on Shutdown, then the caller
        // also destroys its own handle that was the same sampler passed
        // via SetTextureArrays).
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Double-free sampler handle=" << static_cast<u32>(handle)
                  << " — skipping" << std::endl;
        return;
    }
    s->Destroy();
    samplerAllocator_->Free(static_cast<u32>(handle));
}

VulkanDescriptorSetLayout* VulkanDevice::GetDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == handles::INVALID_RESOURCE || !descriptorSetLayoutAllocator_) return nullptr;
    return descriptorSetLayoutAllocator_->Get(static_cast<u32>(handle));
}

// P0.1 诊断:PRIMAL_DS_TRACE=1 启用描述符集/布局追踪(set 误用与 03047 定罪)
static bool DSTraceEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("PRIMAL_DS_TRACE");
        return e && e[0] == '1';
    }();
    return on;
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
    // P0.1 诊断:PRIMAL_DS_TRACE=1 时打印 DSL 的绑定布局(与 [DS-UPD]/错误
    // 消息里的 layout/set 句柄对照,定位 set 误用)。
    if (DSTraceEnabled()) {
        fprintf(stderr, "[DS-LAYOUT] layout=%u bindings=", id);
        for (u32 b = 0; b < desc.bindingCount; ++b)
            fprintf(stderr, "{%u,t%u}", desc.bindings[b].binding,
                    static_cast<unsigned>(desc.bindings[b].descriptorType));
        fprintf(stderr, "\n");
    }
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

DescriptorSetHandle VulkanDevice::createDescriptorSetImpl(const DescriptorSetDesc& desc) {    if (desc.layout == handles::INVALID_RESOURCE) return handles::INVALID_RESOURCE;
    u32 id = descriptorSetAllocator_->Allocate(*this, desc);
    VulkanDescriptorSet* s = descriptorSetAllocator_->Get(id);
    if (!s || !s->Initialize()) {
        if (s) s->destroyImpl();
        descriptorSetAllocator_->Free(id);
        return handles::INVALID_RESOURCE;
    }
    s->SetHandle(ResourceHandle(id));
    // P0.1 诊断:PRIMAL_DS_TRACE=1 时打印 set 的 id 与所属 layout id,
    // 与 [DS-LAYOUT]/[DS-UPD] 对照定位 set 误用。
    if (DSTraceEnabled()) {
        fprintf(stderr, "[DS-NEW] set=%u layout=%u\n", id,
                static_cast<unsigned>(desc.layout));
    }
    return DescriptorSetHandle(id);
}

void VulkanDevice::destroyDescriptorSetImpl(DescriptorSetHandle handle) {
    if (handle == handles::INVALID_RESOURCE) return;
    VulkanDescriptorSet* s = GetDescriptorSet(handle);
    // P0.1 诊断:PRIMAL_DS_TRACE=1 时打印 set 销毁(与 [DS-UPD]/[DS-NEW]
    // 对照,定位"在飞被销毁"的 0x29xx 失效源)。
    if (s && DSTraceEnabled()) {
        fprintf(stderr, "[DS-DEL] set=%p rhi=%u\n",
                (void*)s->GetNativeSet(), static_cast<unsigned>(handle));
    }
    if (s) s->destroyImpl();
    descriptorSetAllocator_->Free(static_cast<u32>(handle));
}

namespace {

// R7-WIN 诊断(00327):physical device 的 minUniformBufferOffsetAlignment
// 按 physicalDevice_ 句柄缓存(GTX1060 为 256;MoltenVK 较小),避免每帧
// updateDescriptorSets 路径反复 vkGetPhysicalDeviceProperties。返回 0
// 表示未知(跳过告警)。仅诊断用,RHI 更新单线程。
VkDeviceSize CachedMinUniformBufferOffsetAlignment(VkPhysicalDevice pd) {
    static VkPhysicalDevice sCachedPd = VK_NULL_HANDLE;
    static VkDeviceSize sCachedAlign = 0;
    if (pd != VK_NULL_HANDLE && pd != sCachedPd) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        sCachedPd = pd;
        sCachedAlign = props.limits.minUniformBufferOffsetAlignment;
    }
    return (pd == sCachedPd) ? sCachedAlign : 0;
}

} // namespace

void VulkanDevice::updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes) {
    if (!writes || writeCount == 0) return;
    // P0.1 诊断:PRIMAL_DS_TRACE=1 时打印 set 更新(原生句柄与 MoltenVK
    // validation 消息里的 VkDescriptorSet 句柄同源,可对照定罪 03047)。
    const bool dsTrace = DSTraceEnabled();
    // 按 dstSet 分组(每个 DescriptorSet::Update 单独处理自己)
    for (u32 i = 0; i < writeCount; ++i) {
        // R7-WIN 诊断(00327):UBO/UBO_DYNAMIC 子区间 offset 未按
        // minUniformBufferOffsetAlignment 对齐的旧写入(如 0x40/0x50)在
        // 桌面驱动上必触发验证错误。有界告警(前 10 次 + 汇总一次),
        // 纯日志,不改变任何写入行为;Mac 正常路径 offset 均为 0,不触发。
        if ((writes[i].descriptorType == DescriptorType::UniformBuffer ||
             writes[i].descriptorType == DescriptorType::UniformBufferDynamic) &&
            writes[i].bufferInfo) {
            const VkDeviceSize uboAlign = CachedMinUniformBufferOffsetAlignment(physicalDevice_);
            if (uboAlign > 1) {
                // 与 VulkanDescriptorSet::Update 同口径:descriptorCount==0 按 1 处理
                const u32 uboN = std::max<u32>(1u, writes[i].descriptorCount);
                for (u32 j = 0; j < uboN; ++j) {
                    const u64 uboOff = writes[i].bufferInfo[j].offset;
                    if ((uboOff % uboAlign) != 0) {
                        static u32 sUnalignedUboWarns = 0;
                        if (sUnalignedUboWarns < 10) {
                            ++sUnalignedUboWarns;
                            PRIMAL_LOG_STREAM(Warn, "RHI")
                                << "[VulkanDevice] unaligned UBO offset in UpdateDescriptorSets: set="
                                << static_cast<unsigned>(writes[i].dstSet)
                                << " binding=" << writes[i].dstBinding
                                << " offset=0x" << std::hex << uboOff
                                << " not multiple of minUniformBufferOffsetAlignment=0x"
                                << uboAlign << std::dec
                                << " (VUID-VkWriteDescriptorSet-descriptorType-00327 risk)"
                                << std::endl;
                            if (sUnalignedUboWarns == 10) {
                                PRIMAL_LOG_STREAM(Warn, "RHI")
                                    << "[VulkanDevice] unaligned UBO offset warnings suppressed after "
                                    << sUnalignedUboWarns << " hits (per-process)" << std::endl;
                            }
                        }
                    }
                }
            }
        }
        VulkanDescriptorSet* ds = GetDescriptorSet(writes[i].dstSet);
        if (ds) {
            if (dsTrace) {
                fprintf(stderr, "[DS-UPD] set=%p rhi=%u binding=%u type=%d\n",
                        (void*)ds->GetNativeSet(),
                        static_cast<unsigned>(writes[i].dstSet),
                        writes[i].dstBinding,
                        static_cast<int>(writes[i].descriptorType));
            }
            ds->Update(&writes[i], 1);
        }
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] vmaCreateAllocator failed: " << res << std::endl;
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
    // 单点汇聚重定向（CLI 规划 §11.3 轨道 B）：验证层消息同步进 diag 通道，
    // EngineDLL 注入发射器后宿主（编辑器面板/CLI/黑匣子）可达；未注入时空操作。
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        std::string msg = std::string(tag) + " ERROR: " + (data->pMessage ? data->pMessage : "");
        primal::diag::Emit(primal::diag::kLevelError, "RHI", msg.c_str());
        PRIMAL_LOG_STREAM(Error, "RHI") << msg << std::endl;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::string msg = std::string(tag) + " WARN:  " + (data->pMessage ? data->pMessage : "");
        primal::diag::Emit(primal::diag::kLevelWarn, "RHI", msg.c_str());
        PRIMAL_LOG_STREAM(Error, "RHI") << msg << std::endl;
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
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] Validation requested but VK_LAYER_KHRONOS_validation not present" << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] vkCreateInstance failed: " << result << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] vkCreateDebugUtilsMessengerEXT not loaded" << std::endl;
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

    PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Debug messenger initialized." << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] No Vulkan-capable physical devices" << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] No suitable physical device" << std::endl;
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Picked physical device: " << props.deviceName << std::endl;
    // Multiple ICDs can expose the same GPU name. Record the selected driver's
    // identity so captures cannot silently attribute another compiler's output.
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 details{};
        details.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        details.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physicalDevice_, &details);
        PRIMAL_LOG_STREAM(Info, "RHI") << "[VulkanDevice] Driver: driver_id=" << u32(driver.driverID)
            << " api_version=" << props.apiVersion << " driver_version=" << props.driverVersion
            << " vendor_id=" << props.vendorID << " device_id=" << props.deviceID
            << " name=" << driver.driverName << " info=" << driver.driverInfo << std::endl;
    }

    return true;
}

bool VulkanDevice::createLogicalDevice() {
    graphicsQueueFamily_ = findQueueFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    if (graphicsQueueFamily_ == UINT32_MAX) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] No graphics/compute queue family found" << std::endl;
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
    // VT M2.14: 片元级 feedback 写入(SSBO store)— 设计文档 §7 能力 #2
    if (supported.fragmentStoresAndAtomics) features.fragmentStoresAndAtomics = VK_TRUE;
    // Phase 4+ 在 features2 链中开 mesh shader / raytracing / bindless。

    std::vector<const char*> deviceExtensions;
    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#if defined(__APPLE__)
    // MoltenVK portability subset is mandatory on macOS.
    deviceExtensions.push_back("VK_KHR_portability_subset");
#endif

    // P4c-F5: layered 渲染的 vertex-shader gl_Layer 写出需要该能力
    // (Vulkan 1.2 core;旧实例上以扩展形式提供)。按支持情况启用。
    bool hasExtShaderViewportIndexLayer = false;
    // T6.3 sub-B: descriptor indexing(数组描述符 + partially-bound +
    // SSBO 数组非一致索引)。Vulkan 1.2 core;旧设备上以
    // VK_EXT_descriptor_indexing 扩展提供。枚举一次,两处共用。
    bool hasExtDescriptorIndexing = false;
    {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> avail(extCount);
        if (extCount > 0) {
            vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extCount, avail.data());
        }
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) == 0) {
                hasExtShaderViewportIndexLayer = true;
                break;
            }
        }
        if (hasExtShaderViewportIndexLayer)
            deviceExtensions.push_back(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0) {
                hasExtDescriptorIndexing = true;
                break;
            }
        }
        // R12-3.2（Windows GI-off f301 AV 诊断）：PRIMAL_VK_MEM_BUDGET_DIAG=1
        // 时启用 VK_EXT_memory_budget——heapUsage/heapBudget 只有扩展启用后才
        // 保证有效。默认关闭时零行为变化（不加扩展、不查询）。
        if (hangdiag::BudgetDiagEnabled()) {
            for (const auto& e : avail) {
                if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                    deviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                    break;
                }
            }
        }
    }

    // T6.3 sub-B: 查询并启用 descriptor indexing 特性组合。N-volume GI gather
    // (GIGatherModule binding 17 SSBO 数组)需要:
    //   - descriptorBindingPartiallyBound:volume 数 < 4 时未写槽位合法;
    //   - shaderStorageBufferArrayNonUniformIndexing:逐像素 volume 选择是非
    //     一致索引(shader 内 nonuniformEXT 标注)。
    // 能力不足时不开特性、capability 位保持 false,消费者回退两槽路径。
    VkPhysicalDeviceVulkan12Features vk12Features{};
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT indexingFeaturesEXT{};
    {
        VkPhysicalDeviceProperties physProps{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &physProps);
        const bool api12 = physProps.apiVersion >= VK_API_VERSION_1_2;
        // 查询链同时挂 Vulkan12(1.2+ core)与 EXT(1.0/1.1 设备)两个结构,
        // 驱动按 sType 各自识别填充;两布局不同,不能混读。
        VkPhysicalDeviceDescriptorIndexingFeaturesEXT supportedEXT{};
        supportedEXT.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
        VkPhysicalDeviceVulkan12Features supported12{};
        supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &supported12;
        supported12.pNext = &supportedEXT;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &f2);

        bool ok = false;
        if (api12) {
            ok = supported12.descriptorIndexing == VK_TRUE &&
                 supported12.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
                 supported12.descriptorBindingPartiallyBound == VK_TRUE;
        } else if (hasExtDescriptorIndexing) {
            deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            ok = supportedEXT.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
                 supportedEXT.descriptorBindingPartiallyBound == VK_TRUE;
        }
        if (ok) {
            if (api12) {
                vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
                vk12Features.descriptorIndexing = VK_TRUE;
                vk12Features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
                vk12Features.descriptorBindingPartiallyBound = VK_TRUE;
                // VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02835:
                // Vulkan12Features 进 pNext 链后,若同时启用了
                // VK_EXT_shader_viewport_index_layer(上方按扩展存在性加入),
                // 必须把对应两个 core 特性一并置 VK_TRUE。不支持时移除该
                // 扩展(1.2+ 设备上分层渲染走 core 路径,不依赖扩展)。
                if (hasExtShaderViewportIndexLayer) {
                    if (supported12.shaderOutputViewportIndex == VK_TRUE &&
                        supported12.shaderOutputLayer == VK_TRUE) {
                        vk12Features.shaderOutputViewportIndex = VK_TRUE;
                        vk12Features.shaderOutputLayer = VK_TRUE;
                    } else {
                        for (auto it = deviceExtensions.begin(); it != deviceExtensions.end(); ) {
                            if (std::strcmp(*it, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) == 0)
                                it = deviceExtensions.erase(it);
                            else
                                ++it;
                        }
                    }
                }
                descriptorIndexingSupported_ = true;
            } else {
                indexingFeaturesEXT.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
                indexingFeaturesEXT.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
                indexingFeaturesEXT.descriptorBindingPartiallyBound = VK_TRUE;
                descriptorIndexingSupported_ = true;
            }
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] descriptor indexing enabled ("
                      << (api12 ? "Vulkan 1.2+ core" : "VK_EXT_descriptor_indexing")
                      << ") — N-volume GI gather available" << std::endl;
        }
    }

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
    if (descriptorIndexingSupported_) {
        // vk12Features(1.2+ core)与 indexingFeaturesEXT(1.0/1.1)二选一,
        // 见上方能力查询块。
        if (vk12Features.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            ci.pNext = &vk12Features;
        } else {
            ci.pNext = &indexingFeaturesEXT;
        }
    }

    if (vkCreateDevice(physicalDevice_, &ci, nullptr, &device_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanDevice] vkCreateDevice failed" << std::endl;
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

// P0-B: one-shot UNDEF→SRO priming（VulkanTexture 首帧绑定 SampledImage 的
// 布局契约）。资产加载期/初始化期调用；帧录制期调用无意义（帧内 barrier
// 走 truth-based tracked 路径）。
void VulkanDevice::PrimeTextureLayout(rhi::ResourceHandle texture) {
    VulkanTexture* tex = GetTexture(texture);
    if (!tex || !tex->GetNativeImage()) return;
    if (tex->GetCurrentLayout() != VK_IMAGE_LAYOUT_UNDEFINED) return; // 已有明确状态
    VkDevice dev = GetNativeDevice();
    VkQueue queue = GetGraphicsQueue();
    u32 queueFamily = GetGraphicsQueueFamily();
    if (queue == VK_NULL_HANDLE) return;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = queueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) return;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(dev, pool, nullptr);
        return;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = tex->GetNativeImage();
    bar.subresourceRange.aspectMask = tex->GetAspectMask();
    bar.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    bar.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    {
        // R10 taskC：queueWaitIdle 门控看门狗（PrimeToShaderRead 同步上传）。
        hangdiag::ScopedHangWatch wait("queueWaitIdle", "VulkanDevice.PrimeToShaderRead");
        vkQueueWaitIdle(queue);
    }
    vkDestroyCommandPool(dev, pool, nullptr);

    tex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (std::getenv("PRIMAL_RG_LAYOUT_TRACE")) {
        fprintf(stderr, "[PRIME] img=0x%llx primed to SRO\n",
                (unsigned long long)(uintptr_t)tex->GetNativeImage());
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
