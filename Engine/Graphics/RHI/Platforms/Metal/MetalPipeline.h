/**
 * @file MetalPipeline.h
 * @brief Metal管线实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"
#include "../../Core/RHIDevice.h" // For GraphicsPipelineDesc/ComputePipelineDesc

namespace primal::graphics::rhi {

class MetalDevice;
class MetalShader;

class MetalPipeline {
public:
    explicit MetalPipeline(MetalDevice& device);
    ~MetalPipeline();

    // Initialize graphics pipeline
    bool Initialize(const GraphicsPipelineDesc& desc);

    // Initialize compute pipeline
    bool Initialize(const ComputePipelineDesc& desc);

    void Destroy();

    MTL::RenderPipelineState* GetRenderPipelineState() const { return renderPipelineState_; }
    MTL::ComputePipelineState* GetComputePipelineState() const { return computePipelineState_; }
    MTL::DepthStencilState* GetDepthStencilState() const { return depthStencilState_; }
    MTL::Size GetThreadGroupSize() const { return threadGroupSize_; }
    
    // Helper to convert primitive topology
    static MTL::PrimitiveType ToMTLPrimitiveType(PrimitiveTopology topology);
    
    const GraphicsPipelineDesc& GetGraphicsDesc() const { return graphicsDesc_; }
    const ComputePipelineDesc& GetComputeDesc() const { return computeDesc_; }
    bool IsCompute() const { return isCompute_; }

    void SetHandle(PipelineHandle handle) { handle_ = handle; }
    PipelineHandle GetHandle() const { return handle_; }

private:
    // Helper methods for state creation
    MTL::DepthStencilState* CreateDepthStencilState(const GraphicsPipelineDesc& desc);
    MTL::VertexDescriptor* CreateVertexDescriptor(const GraphicsPipelineDesc& desc);
    MTL::RenderPipelineDescriptor* CreateRenderPipelineDescriptor(const GraphicsPipelineDesc& desc, MTL::VertexDescriptor* vertexDesc);

    MetalDevice& device_;
    PipelineHandle handle_ = handles::INVALID_PIPELINE;
    
    bool isCompute_ = false;
    GraphicsPipelineDesc graphicsDesc_;
    ComputePipelineDesc computeDesc_;
    
    // Cached thread group size for compute dispatch
    MTL::Size threadGroupSize_;

    MTL::RenderPipelineState* renderPipelineState_ = nullptr;
    MTL::ComputePipelineState* computePipelineState_ = nullptr;
    MTL::DepthStencilState* depthStencilState_ = nullptr;
};

} // namespace primal::graphics::rhi
