/**
 * @file MetalCommandBuffer.h
 * @brief Metal 命令缓冲区实现
 * @details 实现 Metal 平台的命令记录和提交
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHICommand.h"
#include <Metal/Metal.hpp>

namespace primal::graphics::rhi {

class MetalDevice;

/**
 * @brief Metal 命令缓冲区
 */
class MetalCommandBuffer : public RHICommandBuffer {
    friend class MetalDevice;
public:
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param type 命令队列类型
     */
    MetalCommandBuffer(MetalDevice& device, CommandQueueType type);
    
    /**
     * @brief 析构函数
     */
    ~MetalCommandBuffer() override;

    // === RHICommandBuffer 接口实现 ===
    
    bool Initialize() override;
    
protected:
    void destroyImpl() override;
    bool resetImpl() override;
    bool beginImpl() override;
    bool endImpl() override;
    bool submitImpl(u32 waitFlags) override;
    bool waitForCompletionImpl() override;

public:
    // === 渲染命令 ===
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void BeginRenderPass(RenderPassHandle renderPass) override;
    
    /**
     * @brief 开始并行渲染通道
     * @details 使用 MTLParallelRenderCommandEncoder 并行记录渲染命令
     */
    void BeginParallelRenderPass(const RenderPassDesc& desc);
    void BeginParallelRenderPass(RenderPassHandle renderPass);

    /**
     * @brief 创建并行子命令缓冲区
     * @return 用于并行记录的子命令缓冲区
     */
    MetalCommandBuffer* CreateSecondaryCommandBuffer();

    void EndRenderPass() override;
    void SetViewport(const ViewportDesc& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void BindGraphicsPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets) override;
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset) override;
    void BindDescriptorSets(PipelineBindPoint bindPoint,
                           PipelineLayoutHandle pipelineLayout,
                           u32 firstSet,
                           u32 setCount,
                           const DescriptorSetHandle* descriptorSets,
                           u32 dynamicOffsetCount,
                           const u32* dynamicOffsets) override;
    void PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                      u32 offset, u32 size, const void* pValues) override;
    void WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) override;
    void Draw(u32 vertexCount, u32 startVertex, u32 instanceCount, u32 startInstance) override;
    void DrawIndexed(u32 indexCount, u32 startIndex, u32 baseVertex, u32 instanceCount, u32 startInstance) override;
    void DrawIndirect(ResourceHandle buffer, u64 offset, u32 drawCount) override;

    // === 计算命令 ===
    void BindComputePipeline(PipelineHandle pipeline) override;
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(ResourceHandle buffer, u64 offset) override;
    
    // Metal 特有扩展
    void BindComputeBuffers(u32 firstSlot, u32 slotCount, const ResourceHandle* buffers, const u64* offsets);

    MTL::RenderCommandEncoder* GetCurrentRenderEncoder() const { return (MTL::RenderCommandEncoder*)currentEncoder_; }

    // === 资源命令 ===
    void CopyBuffer(ResourceHandle src, ResourceHandle dst, u64 srcOffset, u64 dstOffset, u64 size) override;
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, u32 regionCount) override;
    void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, u32 regionCount, FilterMode filter) override;
    void GenerateMipmaps(ResourceHandle texture) override;
    void InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) override;

private:
    SyncHandle guardEventHandle_ = handles::INVALID_SYNC;
    u64 guardValue_ = 0;

    /**
     * @brief 结束当前编码器
     * @details 如果有活跃的编码器，结束它
     */
    void endCurrentEncoder();

    /**
     * @brief 获取或创建Blit编码器
     */
    MTL::BlitCommandEncoder* getBlitEncoder();
    
    /**
     * @brief 获取或创建Compute编码器
     */
    MTL::ComputeCommandEncoder* getComputeEncoder();
    
    // 注意: RenderEncoder必须通过BeginRenderPass创建
    /**
     * @brief 私有构造函数，用于创建Secondary CommandBuffer
     */
    MetalCommandBuffer(MetalDevice& device, CommandQueueType type, MTL::RenderCommandEncoder* encoder);

    MTL::CommandBuffer* mtlCommandBuffer_{nullptr}; ///< Metal命令缓冲区
    NS::AutoreleasePool* pool_{nullptr};            ///< 自动释放池
     // 并行渲染支持
    MTL::ParallelRenderCommandEncoder* parallelRenderEncoder_{nullptr};
    bool isSecondary_{false};
    
    // 当前活动编码器状态
    enum class EncoderType { None, Render, Compute, Blit };
    EncoderType currentEncoderType_{EncoderType::None};
    MTL::CommandEncoder* currentEncoder_{nullptr};
    
    // 渲染状态跟踪
    MTL::PrimitiveType currentPrimitiveType_{MTL::PrimitiveTypeTriangle};
    MTL::IndexType currentIndexType_{MTL::IndexTypeUInt32};
    MTL::Buffer* currentIndexBuffer_{nullptr};
    NS::UInteger currentIndexBufferOffset_{0};
    
    // 计算状态跟踪
    MTL::Size currentThreadGroupSize_{1, 1, 1};

    // 线程调试
    std::thread::id recordingThreadId_;
};

} // namespace primal::graphics::rhi
