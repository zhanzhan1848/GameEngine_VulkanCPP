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
    bool submitImpl(uint32_t waitFlags) override;
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
    void BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) override;
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset) override;
    void BindDescriptorSets(PipelineBindPoint bindPoint,
                           PipelineLayoutHandle pipelineLayout,
                           uint32_t firstSet,
                           uint32_t setCount,
                           const DescriptorSetHandle* descriptorSets,
                           uint32_t dynamicOffsetCount,
                           const uint32_t* dynamicOffsets) override;
    void Draw(uint32_t vertexCount, uint32_t startVertex, uint32_t instanceCount, uint32_t startInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex, uint32_t baseVertex, uint32_t instanceCount, uint32_t startInstance) override;
    void DrawIndirect(ResourceHandle buffer, uint64_t offset, uint32_t drawCount) override;

    // === 计算命令 ===
    void BindComputePipeline(PipelineHandle pipeline) override;
    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
    void DispatchIndirect(ResourceHandle buffer, uint64_t offset) override;
    
    // Metal 特有扩展
    void BindComputeBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets);

    // === 资源命令 ===
    void CopyBuffer(ResourceHandle src, ResourceHandle dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override;
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, uint32_t regionCount) override;
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, uint32_t regionCount) override;
    void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, uint32_t regionCount, FilterMode filter) override;
    void GenerateMipmaps(ResourceHandle texture) override;
    void InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) override;

private:
    SyncHandle guardEventHandle_ = handles::INVALID_SYNC;
    uint64_t guardValue_ = 0;

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
