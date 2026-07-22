#pragma once
#include "DawnCommon.h"
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "../../Core/RHICommand.h"
namespace primal::graphics::rhi {
class DawnDevice;

class DawnCommandBuffer : public RHICommandBuffer {
    friend class DawnDevice;
public:
    DawnCommandBuffer(DawnDevice& device, CommandQueueType type);
    ~DawnCommandBuffer() override;
    bool Initialize() override;

    // Lifecycle
    bool Reset() override;
    bool Begin() override;
    bool End() override;
    bool Submit(u32 waitFlags) override;
    bool WaitForCompletion() override;

    // Render pass
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void BeginRenderPass(RenderPassHandle renderPass) override;
    void EndRenderPass() override;

    // Viewport / Scissor
    void SetViewport(const ViewportDesc& viewport) override;
    void SetScissor(const Rect& scissor) override;

    // Pipeline binding
    void BindGraphicsPipeline(PipelineHandle pipeline) override;
    void BindComputePipeline(PipelineHandle pipeline) override;

    // Resource binding
    void BindVertexBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets) override;
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset) override;
    void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle pipelineLayout, u32 firstSet, u32 setCount, const DescriptorSetHandle* descriptorSets, u32 dynamicOffsetCount, const u32* dynamicOffsets) override;
    void PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags, u32 offset, u32 size, const void* pValues) override;

    // Draw
    void Draw(u32 vertexCount, u32 startVertex, u32 instanceCount, u32 startInstance) override;
    void DrawIndexed(u32 indexCount, u32 startIndex, u32 baseVertex, u32 instanceCount, u32 startInstance) override;
    void DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) override;

    // Compute
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(ResourceHandle buffer, u64 offset) override;
    // WebGPU lacks a direct setBytes equivalent; calls are logged once and ignored.
    // Metal-only callers (Lumen ScreenProbes, PCG) route through BindDescriptorSets on Dawn.
    void SetComputeBytes(u32 index, const void* data, u32 size) override;

    // Barriers (no-op in WebGPU)
    void MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask, AccessFlag srcAccessMask, AccessFlag dstAccessMask) override;
    void InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) override;

    // Resource operations
    void CopyBuffer(ResourceHandle src, ResourceHandle dst, u64 srcOffset, u64 dstOffset, u64 size) override;
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, u32 regionCount, FilterMode filter) override;
    void GenerateMipmaps(ResourceHandle texture) override;

    // Queries
    void WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) override;

    // Access the native WGPU command buffer (used by DawnDevice::submitImpl)
    WGPUCommandBuffer GetNativeCommandBuffer() const { return wgpuCommandBuffer_; }

private:
    // RHICommandBuffer protected virtuals
    void destroyImpl() override;
    bool resetImpl() override;
    bool beginImpl() override;
    bool endImpl() override;
    bool submitImpl(u32 waitFlags) override;
    bool waitForCompletionImpl() override;

    DawnDevice& device_;
    WGPUCommandEncoder wgpuEncoder_ = nullptr;
    WGPURenderPassEncoder wgpuRenderPass_ = nullptr;
    WGPUComputePassEncoder wgpuComputePass_ = nullptr;
    WGPUCommandBuffer wgpuCommandBuffer_ = nullptr;

    enum class EncoderType { None, Render, Compute, Copy };
    EncoderType currentEncoderType_ = EncoderType::None;

    void EnsureCommandEncoder();
    void EndCurrentEncoder();
};
} // namespace
#endif
