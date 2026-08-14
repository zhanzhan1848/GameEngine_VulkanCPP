#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHIDevice.h"
#include "../../Core/RHIAllocator.h"
#include "DawnBuffer.h"
#include "DawnTexture.h"
#include "DawnCommandBuffer.h"
#include "DawnSync.h"
#include "DawnQuery.h"
#include "DawnShader.h"
#include "DawnPipeline.h"
#include "DawnSampler.h"
#include "DawnDescriptorSetLayout.h"
#include "DawnPipelineLayout.h"
#include "DawnDescriptorSet.h"
#include "DawnRenderPass.h"
#include <atomic>
#include <mutex>

namespace primal::graphics::rhi {

class DawnDevice : public RHIDevice<DawnDevice> {
    friend class RHIDevice<DawnDevice>;
    friend class DawnCommandBuffer;

public:
    explicit DawnDevice(const DeviceDesc& desc);
    ~DawnDevice() override;

    WGPUDevice GetNativeDevice() const { return wgpuDevice_; }
    WGPUQueue GetQueue() const { return wgpuQueue_; }
    WGPUAdapter GetAdapter() const { return wgpuAdapter_; }
    WGPUInstance GetInstance() const { return wgpuInstance_; }

    // Internal resource accessors
    DawnBuffer* GetBuffer(ResourceHandle handle);
    DawnTexture* GetTexture(ResourceHandle handle);
    DawnCommandBuffer* GetCommandBuffer(CommandBufferHandle handle);
    DawnSync* GetSync(SyncHandle handle);
    DawnQueryPool* GetQueryPool(QueryPoolHandle handle);
    DawnShader* GetShader(ShaderHandle handle);
    DawnPipeline* GetPipeline(PipelineHandle handle);
    DawnSampler* GetSampler(SamplerHandle handle);
    DawnDescriptorSetLayout* GetDescriptorSetLayout(DescriptorSetLayoutHandle handle);
    DawnPipelineLayout* GetPipelineLayout(PipelineLayoutHandle handle);
    DawnDescriptorSet* GetDescriptorSet(DescriptorSetHandle handle);
    DawnRenderPass* GetRenderPass(RenderPassHandle handle);

    // Wrap a WGPUTexture (e.g. swapchain surface texture) in a DawnTexture
    // so it can be used as a ResourceHandle in render passes.
    ResourceHandle WrapSurfaceTexture(WGPUTexture texture, u32 width, u32 height, DataFormat format);
    void ReleaseSurfaceTexture(ResourceHandle handle);
    void UpdateSurfaceTexture(ResourceHandle handle, WGPUTexture texture, u32 width, u32 height, DataFormat format);

    // Push constant emulation ring buffer
    WGPUBuffer GetPushConstantBuffer() const { return pushConstantBuffer_; }
    u32 AllocatePushConstantSlot();
    void ResetPushConstantOffset();

    // Shared dummy 1x1 magenta texture for missing bindings
    WGPUTextureView GetDummyTextureView();

    // Texture data upload via wgpuQueueWriteTexture
    void UpdateTextureData(ResourceHandle handle, const void* data,
        u32 x, u32 y, u32 z, u32 width, u32 height, u32 depth, u32 rowPitch,
        u32 mipLevel = 0);

    // Buffer data upload via wgpuQueueWriteBuffer. Without this override, the
    // base class default returns false and RenderMesh::Create silently fails
    // to upload vertex/index data — every ForwardPBR/ShadowAndIBL/Deferred
    // draw call then reads uninitialized buffers and the screen renders as
    // the clear color.
    bool UpdateBufferData(ResourceHandle handle, const void* data, u64 size, u64 offset = 0) override;

    // Flush all persistently mapped staging buffers (call before direct wgpuQueueSubmit)
    void FlushStagingBuffers();

protected:
    // CRTP implementation interface
    bool initializeImpl();
    void shutdownImpl();
    void waitIdleImpl() const;
    void beginFrameImpl();
    void endFrameImpl();
    void presentImpl();
    void queryDeviceInfo(DeviceInfo& info);
    u32 getCurrentFrameIndexImpl() const;

    // Command submission
    bool submitImpl(const QueueSubmitInfo& info);

    // Sync objects
    SyncHandle createSyncImpl();
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs);
    void destroySyncImpl(SyncHandle handle);

    // Query pools
    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc);
    void destroyQueryPoolImpl(QueryPoolHandle handle);
    bool getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride);
    double getTimestampPeriodImpl() const;

    // SwapChain
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc);
    void destroySwapChainImpl(RHISwapChain* swapChain);

    // Resource creation
    ResourceHandle createBufferImpl(const BufferDesc& desc);
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

    // Resource destruction
    void destroyBufferImpl(ResourceHandle handle);
    void destroyTextureImpl(ResourceHandle handle);
    void destroyShaderImpl(ShaderHandle handle);
    void destroyPipelineImpl(PipelineHandle handle);
    void destroyCommandBufferImpl(CommandBufferHandle handle);
    void destroySamplerImpl(SamplerHandle handle);
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle);
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle);
    void destroyDescriptorSetImpl(DescriptorSetHandle handle);
    void destroyRenderPassImpl(RenderPassHandle handle);

    // Memory operations
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size);
    void unmapBufferImpl(ResourceHandle handle);
    void setBufferDirtySizeImpl(ResourceHandle handle, u64 size);

private:
    WGPUInstance wgpuInstance_{nullptr};
    WGPUAdapter wgpuAdapter_{nullptr};
    WGPUDevice wgpuDevice_{nullptr};
    WGPUQueue wgpuQueue_{nullptr};

    // Push constant emulation
    WGPUBuffer pushConstantBuffer_{nullptr};
    static constexpr u32 PUSH_CONSTANT_RING_SIZE = 256 * 1024;  // 256 KB ring buffer
    static constexpr u32 PUSH_CONSTANT_ALIGNMENT = 256;
    std::atomic<u32> pushConstantOffset_{0};

    std::atomic<u32> currentFrameIndex_{0};

    // Allocators for all resource types
    RHIAllocator<DawnBuffer> bufferAllocator_;
    RHIAllocator<DawnTexture> textureAllocator_;
    RHIAllocator<DawnCommandBuffer> commandBufferAllocator_;
    RHIAllocator<DawnSync> syncAllocator_;
    RHIAllocator<DawnQueryPool> queryPoolAllocator_;
    RHIAllocator<DawnShader> shaderAllocator_;
    RHIAllocator<DawnPipeline> pipelineAllocator_;
    RHIAllocator<DawnSampler> samplerAllocator_;
    RHIAllocator<DawnDescriptorSetLayout> descriptorSetLayoutAllocator_;
    RHIAllocator<DawnPipelineLayout> pipelineLayoutAllocator_;
    RHIAllocator<DawnDescriptorSet> descriptorSetAllocator_;
    RHIAllocator<DawnRenderPass> renderPassAllocator_;

    // Device error/lost callbacks are now set inline in WGPUDeviceDescriptor
    // via WGPUDeviceLostCallbackInfo and WGPUUncapturedErrorCallbackInfo

    // Dummy 1x1 texture for missing texture bindings
    WGPUTexture dummyTexture_{nullptr};
    WGPUTextureView dummyTextureView_{nullptr};

    // Blit pipeline for mipmap generation (lazy-initialized)
    WGPURenderPipeline blitPipeline_{nullptr};
    WGPUBindGroupLayout blitBindGroupLayout_{nullptr};
    WGPUSampler blitSampler_{nullptr};
    bool blitPipelineInitialized_{false};

    bool EnsureBlitPipeline();
    void DestroyBlitPipeline();
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
