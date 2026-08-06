/**
 * @file VulkanDevice.h
 * @brief Vulkan 设备实现（CRTP）
 * @details Phase 1: 仅 VkInstance + VkPhysicalDevice + VkDevice + 三 queue + debug messenger。
 *          Phase 2 起加入 VMA allocator 和资源 allocators。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 * @version 0.1.0
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <memory>
#include <mutex>
#include <unordered_map>

#include "../../Core/RHIDevice.h"
#include "../../Core/RHIAllocator.h"
#include "VulkanBuffer.h"
#include "VulkanSync.h"
#include "VulkanQuery.h"
#include "VulkanStagingAllocator.h"
#include "VulkanTexture.h"

namespace primal::graphics::rhi {

// Phase 3 前向声明(定义在各自 .h,include 在 .cpp 内避免循环)
class VulkanCommandBuffer;
// Phase 4 前向声明
class VulkanShader;
class VulkanSampler;
class VulkanDescriptorSetLayout;
class VulkanPipelineLayout;
class VulkanDescriptorSet;
class VulkanPipeline;
class VulkanRenderPass;

/**
 * @brief Vulkan 设备实现类
 * @details 继承 RHIDevice<VulkanDevice>，CRTP 静态分发。
 *          Phase 1: instance / physical / logical device / queues / debug messenger。
 *          Phase 2: VMA allocator + VulkanBuffer + VulkanSync + VulkanQuery + Staging。
 *          Phase 3: VulkanTexture + VulkanCommandBuffer(Copy/Blit/Barrier/Mipmap)。
 *          Phase 4+: shader、pipeline、swapchain 等逐步填充。
 */
class VulkanDevice : public RHIDevice<VulkanDevice> {
    friend class RHIDevice<VulkanDevice>;
    friend class VulkanBuffer;
    friend class VulkanTexture;
    friend class VulkanCommandBuffer;
    friend class VulkanShader;
    friend class VulkanSampler;
    friend class VulkanDescriptorSetLayout;
    friend class VulkanPipelineLayout;
    friend class VulkanDescriptorSet;
    friend class VulkanPipeline;
    friend class VulkanRenderPass;
    friend class VulkanSwapChain;

public:
    explicit VulkanDevice(const DeviceDesc& desc);
    ~VulkanDevice() override;

    VkInstance GetNativeInstance() const { return instance_; }
    VkPhysicalDevice GetNativePhysicalDevice() const { return physicalDevice_; }
    VkDevice GetNativeDevice() const { return device_; }
    VkQueue GetGraphicsQueue() const { return graphicsQueue_; }
    VkQueue GetComputeQueue() const { return computeQueue_; }
    VkQueue GetTransferQueue() const { return transferQueue_; }
    u32 GetGraphicsQueueFamily() const { return graphicsQueueFamily_; }

    /// VMA 实例(给 VulkanBuffer/VulkanStagingAllocator 等用)
    VmaAllocator GetVmaAllocator() const { return vmaAllocator_; }
    VulkanStagingAllocator& GetStagingAllocator() { return stagingAllocator_; }

    /// Phase 4b: SwapChain 需要把 wrap 模式的 backbuffer 注册到 allocator,
    /// 这样 InsertBarrier(via GetTexture(handle)) 才能找到。
    RHIAllocator<VulkanTexture>& GetTextureAllocator() { return *textureAllocator_; }

    /// Debug utils 设对象名函数(给资源挂 label)
    PFN_vkSetDebugUtilsObjectNameEXT GetDebugUtilsSetObjectName() const {
        return vkSetDebugUtilsObjectName_;
    }

    /// Phase 3: Per-type lookup(tests + 外部 caller 用)
    /// Buffer/Sync/QueryPool 在 protected 区域,future 可能 hoist 出来
    VulkanTexture* GetTexture(ResourceHandle handle);
    VulkanCommandBuffer* GetCommandBuffer(CommandBufferHandle handle);

    /// Phase 4: Per-type lookup
    VulkanShader*              GetShader(ShaderHandle handle);
    VulkanSampler*             GetSampler(SamplerHandle handle);
    VulkanDescriptorSetLayout* GetDescriptorSetLayout(DescriptorSetLayoutHandle handle);
    VulkanPipelineLayout*      GetPipelineLayout(PipelineLayoutHandle handle);
    VulkanDescriptorSet*       GetDescriptorSet(DescriptorSetHandle handle);
    VulkanPipeline*            GetPipeline(PipelineHandle handle);
    VulkanRenderPass*          GetRenderPass(RenderPassHandle handle);

    // T4.6.5 part 32: GetSync hoisted to public so tests can call ResetFence()
    // on a freshly-created fence to validate timeout behavior. Previously
    // protected with comment "future 可能 hoist 出来" — that future is now.
    VulkanSync* GetSync(SyncHandle handle) {
        if (handle == handles::INVALID_SYNC) return nullptr;
        return syncAllocator_.Get(static_cast<u32>(handle));
    }

protected:
    // === CRTP 实现接口 ===
    bool initializeImpl();
    void shutdownImpl();
    void waitIdleImpl() const;
    void beginFrameImpl();
    void endFrameImpl();
    void presentImpl();
    void queryDeviceInfo(DeviceInfo& info);
    u32 getCurrentFrameIndexImpl() const;
    double getTimestampPeriodImpl() const { return timestampPeriodNs_; }

    // === Per-type lookup(friend 子类用) ===
    VulkanBuffer* GetBuffer(ResourceHandle handle) {
        if (handle == handles::INVALID_RESOURCE) return nullptr;
        return bufferAllocator_.Get(static_cast<u32>(handle));
    }
    VulkanQueryPool* GetQueryPool(QueryPoolHandle handle) {
        if (handle == handles::INVALID_QUERY_POOL) return nullptr;
        return queryPoolAllocator_.Get(static_cast<u32>(handle));
    }

    // === Phase 2 资源创建 ===
    SyncHandle createSyncImpl();
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs);
    void destroySyncImpl(SyncHandle handle);

    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc);
    void destroyQueryPoolImpl(QueryPoolHandle handle);
    bool getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride);

    ResourceHandle createBufferImpl(const BufferDesc& desc);
    void destroyBufferImpl(ResourceHandle handle);
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size);
    void unmapBufferImpl(ResourceHandle handle);
    void setBufferDirtySizeImpl(ResourceHandle handle, u64 size);

    // ODR-bypass:跨 dylib 边界时,RHIResource singleton 分裂,必须经设备本地查表
    bool UpdateBufferData(ResourceHandle handle, const void* data, u64 size, u64 offset = 0) override;

    // T4.6.5 part 24.2 (B6 fix): Vulkan-spec query pool reset. Override base
    // virtual directly (no CRTP shim) — Metal/Dawn inherit empty default.
    void ResetQueryPool(QueryPoolHandle handle, u32 firstQuery, u32 queryCount) override;

    // === Phase 5: Shader 热重载 ===
    bool ReloadShader(ShaderHandle shader, const void* data, size_t size) override;
    void RegisterPipelineDependency(PipelineHandle pipeline, ShaderHandle shader);
    void UnregisterPipelineDependency(PipelineHandle pipeline);

    // === Phase 3+ 占位实现（仍返回 INVALID / no-op）===
    bool submitImpl(const QueueSubmitInfo& info);
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc);
    void destroySwapChainImpl(RHISwapChain* swapChain);

    ResourceHandle createTextureImpl(const TextureDesc& desc);
    ResourceHandle createTextureViewImpl(const TextureViewDesc& desc);
    ShaderHandle createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint);

    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc);
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc& desc);
    CommandBufferHandle createCommandBufferImpl(CommandQueueType type);
    SamplerHandle createSamplerImpl(const SamplerDesc& desc);
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc);
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc& desc);
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc& desc);
    void updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes);

    RenderPassHandle createRenderPassImpl(const RenderPassDesc& desc);

    void destroyTextureImpl(ResourceHandle handle);
    void destroyShaderImpl(ShaderHandle handle);
    void destroyPipelineImpl(PipelineHandle handle);
    void destroyCommandBufferImpl(CommandBufferHandle handle);
    void destroySamplerImpl(SamplerHandle handle);
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle);
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle);
    void destroyDescriptorSetImpl(DescriptorSetHandle handle);
    void destroyRenderPassImpl(RenderPassHandle handle);

private:
    bool createInstance();
    void destroyInstance();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    void destroyLogicalDevice();
    bool setupDebugMessenger();
    void teardownDebugMessenger();

    /// Find a queue family that supports `flags`. Returns UINT32_MAX on failure.
    u32 findQueueFamily(VkQueueFlags flags) const;

    /// VMA 实例创建 / 销毁
    bool createVmaAllocator();
    void destroyVmaAllocator();

    // === Native handles ===
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};

    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    VkQueue computeQueue_{VK_NULL_HANDLE};
    VkQueue transferQueue_{VK_NULL_HANDLE};
    u32 graphicsQueueFamily_{UINT32_MAX};
    u32 computeQueueFamily_{UINT32_MAX};
    u32 transferQueueFamily_{UINT32_MAX};

    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectName_{nullptr};

    // === Memory subsystem ===
    VmaAllocator vmaAllocator_{nullptr};
    VulkanStagingAllocator stagingAllocator_;

    // === Per-type free_list allocators ===
    RHIAllocator<VulkanBuffer>   bufferAllocator_;
    RHIAllocator<VulkanSync>     syncAllocator_;
    RHIAllocator<VulkanQueryPool> queryPoolAllocator_;
    // Phase 3: Texture / CommandBuffer allocators — 不完整类型,用 unique_ptr 持有
    // (简化:直接用完整类型,因为 .h 已 include VulkanBuffer.h 模式;Phase 3 改成 ptr 或 fwd declare)
    // 用 forward declare + std::unique_ptr 简化头依赖
    std::unique_ptr<RHIAllocator<VulkanTexture>>    textureAllocator_;
    std::unique_ptr<RHIAllocator<VulkanCommandBuffer>> commandBufferAllocator_;
    // Phase 4 allocators(同样用 unique_ptr 因不完整类型)
    std::unique_ptr<RHIAllocator<VulkanShader>>              shaderAllocator_;
    std::unique_ptr<RHIAllocator<VulkanSampler>>             samplerAllocator_;
    std::unique_ptr<RHIAllocator<VulkanDescriptorSetLayout>> descriptorSetLayoutAllocator_;
    std::unique_ptr<RHIAllocator<VulkanPipelineLayout>>      pipelineLayoutAllocator_;
    std::unique_ptr<RHIAllocator<VulkanDescriptorSet>>       descriptorSetAllocator_;
    std::unique_ptr<RHIAllocator<VulkanPipeline>>            pipelineAllocator_;
    std::unique_ptr<RHIAllocator<VulkanRenderPass>>          renderPassAllocator_;

    float timestampPeriodNs_{1.0f};
    std::atomic<u32> currentFrameIndex_{0};

    // Tracked for shutdown ordering / diagnostics.
    bool validationEnabled_{false};
    bool portabilitySubsetEnabled_{false};

    // === Phase 5: Shader → Pipelines 依赖图(热重载用)===
    std::mutex pipelineDependencyMutex_;
    std::unordered_map<ShaderHandle, std::vector<PipelineHandle>> shaderToPipelines_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
