/**
 * @file MetalPipeline.cpp
 * @brief Metal管线实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#include "MetalPipeline.h"
#include "MetalDevice.h"
#include "MetalShader.h"
#include <iostream>

namespace primal::graphics::rhi {

namespace {
    MTL::PixelFormat ToMTLPixelFormat(DataFormat format) {
        // Simple mapping, needs full implementation
        switch (format) {
            case DataFormat::BGRA8_UNorm: return MTL::PixelFormatBGRA8Unorm;
            case DataFormat::RGBA8_UNorm: return MTL::PixelFormatRGBA8Unorm;
            case DataFormat::R8_UNorm: return MTL::PixelFormatR8Unorm;
            case DataFormat::D32_Float: return MTL::PixelFormatDepth32Float;
            case DataFormat::D32_Float_S8X24_UInt: return MTL::PixelFormatDepth32Float_Stencil8;
            default: return MTL::PixelFormatInvalid;
        }
    }

    MTL::VertexFormat ToMTLVertexFormat(DataFormat format) {
        // Simple mapping, needs full implementation
        switch (format) {
            case DataFormat::RGB32_Float: return MTL::VertexFormatFloat3;
            case DataFormat::RGBA32_Float: return MTL::VertexFormatFloat4;
            case DataFormat::RG32_Float: return MTL::VertexFormatFloat2;
            case DataFormat::R32_Float: return MTL::VertexFormatFloat;
            default: return MTL::VertexFormatInvalid;
        }
    }

    MTL::CompareFunction ToMTLCompareFunction(ComparisonFunc func) {
        switch (func) {
            case ComparisonFunc::Never: return MTL::CompareFunctionNever;
            case ComparisonFunc::Less: return MTL::CompareFunctionLess;
            case ComparisonFunc::Equal: return MTL::CompareFunctionEqual;
            case ComparisonFunc::LessEqual: return MTL::CompareFunctionLessEqual;
            case ComparisonFunc::Greater: return MTL::CompareFunctionGreater;
            case ComparisonFunc::NotEqual: return MTL::CompareFunctionNotEqual;
            case ComparisonFunc::GreaterEqual: return MTL::CompareFunctionGreaterEqual;
            case ComparisonFunc::Always: return MTL::CompareFunctionAlways;
            default: return MTL::CompareFunctionAlways;
        }
    }

    MTL::BlendOperation ToMTLBlendOperation(BlendOp op) {
        switch (op) {
            case BlendOp::Add: return MTL::BlendOperationAdd;
            case BlendOp::Subtract: return MTL::BlendOperationSubtract;
            case BlendOp::RevSubtract: return MTL::BlendOperationReverseSubtract;
            case BlendOp::Min: return MTL::BlendOperationMin;
            case BlendOp::Max: return MTL::BlendOperationMax;
            default: return MTL::BlendOperationAdd;
        }
    }

    MTL::BlendFactor ToMTLBlendFactor(BlendFactor factor) {
        switch (factor) {
            case BlendFactor::Zero: return MTL::BlendFactorZero;
            case BlendFactor::One: return MTL::BlendFactorOne;
            case BlendFactor::SrcColor: return MTL::BlendFactorSourceColor;
            case BlendFactor::InvSrcColor: return MTL::BlendFactorOneMinusSourceColor;
            case BlendFactor::SrcAlpha: return MTL::BlendFactorSourceAlpha;
            case BlendFactor::InvSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
            case BlendFactor::DestAlpha: return MTL::BlendFactorDestinationAlpha;
            case BlendFactor::InvDestAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
            case BlendFactor::DestColor: return MTL::BlendFactorDestinationColor;
            case BlendFactor::InvDestColor: return MTL::BlendFactorOneMinusDestinationColor;
            case BlendFactor::SrcAlphaSat: return MTL::BlendFactorSourceAlphaSaturated;
            case BlendFactor::BlendFactor: return MTL::BlendFactorBlendColor;
            case BlendFactor::InvBlendFactor: return MTL::BlendFactorOneMinusBlendColor;
            case BlendFactor::Src1Color: return MTL::BlendFactorSource1Color;
            case BlendFactor::InvSrc1Color: return MTL::BlendFactorOneMinusSource1Color;
            case BlendFactor::Src1Alpha: return MTL::BlendFactorSource1Alpha;
            case BlendFactor::InvSrc1Alpha: return MTL::BlendFactorOneMinusSource1Alpha;
            default: return MTL::BlendFactorOne;
        }
    }
}

MetalPipeline::MetalPipeline(MetalDevice& device) : device_(device) {}

MetalPipeline::~MetalPipeline() {
    Destroy();
}

void MetalPipeline::Destroy() {
    if (renderPipelineState_) {
        renderPipelineState_->release();
        renderPipelineState_ = nullptr;
    }
    if (computePipelineState_) {
        computePipelineState_->release();
        computePipelineState_ = nullptr;
    }
    if (depthStencilState_) {
        depthStencilState_->release();
        depthStencilState_ = nullptr;
    }
}

bool MetalPipeline::Initialize(const GraphicsPipelineDesc& desc) {
    graphicsDesc_ = desc;
    isCompute_ = false;

    // Create Depth Stencil State
    depthStencilState_ = CreateDepthStencilState(desc);

    // Create Vertex Descriptor
    MTL::VertexDescriptor* vertexDesc = CreateVertexDescriptor(desc);

    // Create Render Pipeline Descriptor
    MTL::RenderPipelineDescriptor* pipelineDesc = CreateRenderPipelineDescriptor(desc, vertexDesc);
    
    // Create Render Pipeline State
    NS::Error* error = nullptr;
    renderPipelineState_ = device_.GetNativeDevice()->newRenderPipelineState(pipelineDesc, &error);
    
    pipelineDesc->release();
    if (vertexDesc) vertexDesc->release();

    if (!renderPipelineState_) {
        if (error) {
            std::cerr << "[MetalPipeline] Failed to create render pipeline state: " 
                      << error->localizedDescription()->utf8String() << std::endl;
            error->release();
        }
        return false;
    }

    // std::cout << "[MetalPipeline] Render pipeline state created successfully." << std::endl;

    return true;
}

bool MetalPipeline::Initialize(const ComputePipelineDesc& desc) {
    computeDesc_ = desc;
    isCompute_ = true;
    threadGroupSize_ = MTL::Size::Make(desc.threadGroupSize.x, desc.threadGroupSize.y, desc.threadGroupSize.z);

    MetalShader* computeShader = device_.GetShader(desc.computeShader);
    if (!computeShader) {
        std::cerr << "[MetalPipeline] Invalid compute shader handle" << std::endl;
        return false;
    }

    MTL::ComputePipelineDescriptor* pipelineDesc = MTL::ComputePipelineDescriptor::alloc()->init();
    pipelineDesc->setComputeFunction(computeShader->GetFunction());

    NS::Error* error = nullptr;
    computePipelineState_ = device_.GetNativeDevice()->newComputePipelineState(pipelineDesc, MTL::PipelineOptionNone, nullptr, &error);
    
    pipelineDesc->release();

    if (!computePipelineState_) {
        if (error) {
            std::cerr << "[MetalPipeline] Failed to create compute pipeline state: " 
                      << error->localizedDescription()->utf8String() << std::endl;
            error->release();
        }
        return false;
    }

    return true;
}

MTL::DepthStencilState* MetalPipeline::CreateDepthStencilState(const GraphicsPipelineDesc& desc) {
    MTL::DepthStencilDescriptor* depthStencilDesc = MTL::DepthStencilDescriptor::alloc()->init();
    
    if (desc.enableDepthTest) {
        depthStencilDesc->setDepthCompareFunction(ToMTLCompareFunction(desc.depthFunc));
        depthStencilDesc->setDepthWriteEnabled(desc.enableDepthWrite);
    } else {
        depthStencilDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
        depthStencilDesc->setDepthWriteEnabled(false);
    }
    
    // TODO: Stencil support

    MTL::DepthStencilState* state = device_.GetNativeDevice()->newDepthStencilState(depthStencilDesc);
    depthStencilDesc->release();
    return state;
}

MTL::VertexDescriptor* MetalPipeline::CreateVertexDescriptor(const GraphicsPipelineDesc& desc) {
    if (desc.vertexAttributes.empty()) return nullptr;

    MTL::VertexDescriptor* vertexDesc = MTL::VertexDescriptor::alloc()->init();

    // Attributes
    for (const auto& attr : desc.vertexAttributes) {
        MTL::VertexAttributeDescriptor* attrDesc = vertexDesc->attributes()->object(attr.location);
        attrDesc->setFormat(ToMTLVertexFormat(attr.format));
        attrDesc->setOffset(attr.offset);
        attrDesc->setBufferIndex(attr.binding);
    }

    // Layouts
    for (const auto& binding : desc.vertexBindings) {
        MTL::VertexBufferLayoutDescriptor* layoutDesc = vertexDesc->layouts()->object(binding.binding);
        layoutDesc->setStride(binding.stride);
        layoutDesc->setStepFunction(binding.perVertex ? MTL::VertexStepFunctionPerVertex : MTL::VertexStepFunctionPerInstance);
        layoutDesc->setStepRate(1);
    }

    return vertexDesc;
}

MTL::RenderPipelineDescriptor* MetalPipeline::CreateRenderPipelineDescriptor(const GraphicsPipelineDesc& desc, MTL::VertexDescriptor* vertexDesc) {
    MTL::RenderPipelineDescriptor* pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();

    // Shaders
    MetalShader* vs = device_.GetShader(desc.vertexShader);
    MetalShader* ps = device_.GetShader(desc.pixelShader);

    if (vs) pipelineDesc->setVertexFunction(vs->GetFunction());
    if (ps) pipelineDesc->setFragmentFunction(ps->GetFunction());

    // Vertex Descriptor
    if (vertexDesc) {
        pipelineDesc->setVertexDescriptor(vertexDesc);
    }

    // Color Attachments
    for (uint32_t i = 0; i < desc.renderTargetCount; ++i) {
        MTL::RenderPipelineColorAttachmentDescriptor* colorDesc = pipelineDesc->colorAttachments()->object(i);
        colorDesc->setPixelFormat(ToMTLPixelFormat(desc.renderTargetFormats[i]));
        
        if (desc.enableBlend) {
            colorDesc->setBlendingEnabled(true);
            colorDesc->setRgbBlendOperation(ToMTLBlendOperation(desc.colorBlendOp));
            colorDesc->setAlphaBlendOperation(ToMTLBlendOperation(desc.alphaBlendOp));
            colorDesc->setSourceRGBBlendFactor(ToMTLBlendFactor(desc.srcColorBlendFactor));
            colorDesc->setDestinationRGBBlendFactor(ToMTLBlendFactor(desc.dstColorBlendFactor));
            colorDesc->setSourceAlphaBlendFactor(ToMTLBlendFactor(desc.srcAlphaBlendFactor));
            colorDesc->setDestinationAlphaBlendFactor(ToMTLBlendFactor(desc.dstAlphaBlendFactor));
        }
    }

    // Depth Format
    if (desc.depthStencilFormat != DataFormat::Unknown) {
        pipelineDesc->setDepthAttachmentPixelFormat(ToMTLPixelFormat(desc.depthStencilFormat));
        if (desc.depthStencilFormat == DataFormat::D32_Float_S8X24_UInt) {
             pipelineDesc->setStencilAttachmentPixelFormat(ToMTLPixelFormat(desc.depthStencilFormat));
        }
    }

    return pipelineDesc;
}

MTL::PrimitiveType MetalPipeline::ToMTLPrimitiveType(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::PointList: return MTL::PrimitiveTypePoint;
        case PrimitiveTopology::LineList: return MTL::PrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTL::PrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList: return MTL::PrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip: return MTL::PrimitiveTypeTriangleStrip;
        default: return MTL::PrimitiveTypeTriangle;
    }
}

} // namespace primal::graphics::rhi
