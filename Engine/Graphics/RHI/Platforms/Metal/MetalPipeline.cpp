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
        switch (format) {
            case DataFormat::R8_UNorm: return MTL::PixelFormatR8Unorm;
            case DataFormat::R8_SNorm: return MTL::PixelFormatR8Snorm;
            case DataFormat::R8_UInt: return MTL::PixelFormatR8Uint;
            case DataFormat::R8_SInt: return MTL::PixelFormatR8Sint;
            
            case DataFormat::R16_UNorm: return MTL::PixelFormatR16Unorm;
            case DataFormat::R16_SNorm: return MTL::PixelFormatR16Snorm;
            case DataFormat::R16_UInt: return MTL::PixelFormatR16Uint;
            case DataFormat::R16_SInt: return MTL::PixelFormatR16Sint;
            case DataFormat::R16_Float: return MTL::PixelFormatR16Float;
            
            case DataFormat::RG8_UNorm: return MTL::PixelFormatRG8Unorm;
            case DataFormat::RG8_SNorm: return MTL::PixelFormatRG8Snorm;
            case DataFormat::RG8_UInt: return MTL::PixelFormatRG8Uint;
            case DataFormat::RG8_SInt: return MTL::PixelFormatRG8Sint;
            
            case DataFormat::R32_UInt: return MTL::PixelFormatR32Uint;
            case DataFormat::R32_SInt: return MTL::PixelFormatR32Sint;
            case DataFormat::R32_Float: return MTL::PixelFormatR32Float;
            
            case DataFormat::RG16_UNorm: return MTL::PixelFormatRG16Unorm;
            case DataFormat::RG16_SNorm: return MTL::PixelFormatRG16Snorm;
            case DataFormat::RG16_UInt: return MTL::PixelFormatRG16Uint;
            case DataFormat::RG16_SInt: return MTL::PixelFormatRG16Sint;
            case DataFormat::RG16_Float: return MTL::PixelFormatRG16Float;
            
            case DataFormat::RGBA8_UNorm: return MTL::PixelFormatRGBA8Unorm;
            case DataFormat::RGBA8_SNorm: return MTL::PixelFormatRGBA8Snorm;
            case DataFormat::RGBA8_UInt: return MTL::PixelFormatRGBA8Uint;
            case DataFormat::RGBA8_SInt: return MTL::PixelFormatRGBA8Sint;
            case DataFormat::RGBA8_sRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;
            
            case DataFormat::BGRA8_UNorm: return MTL::PixelFormatBGRA8Unorm;
            
            case DataFormat::RG32_UInt: return MTL::PixelFormatRG32Uint;
            case DataFormat::RG32_SInt: return MTL::PixelFormatRG32Sint;
            case DataFormat::RG32_Float: return MTL::PixelFormatRG32Float;
            
            case DataFormat::RGBA16_UNorm: return MTL::PixelFormatRGBA16Unorm;
            case DataFormat::RGBA16_SNorm: return MTL::PixelFormatRGBA16Snorm;
            case DataFormat::RGBA16_UInt: return MTL::PixelFormatRGBA16Uint;
            case DataFormat::RGBA16_SInt: return MTL::PixelFormatRGBA16Sint;
            case DataFormat::RGBA16_Float: return MTL::PixelFormatRGBA16Float;
            
            case DataFormat::RGBA32_UInt: return MTL::PixelFormatRGBA32Uint;
            case DataFormat::RGBA32_SInt: return MTL::PixelFormatRGBA32Sint;
            case DataFormat::RGBA32_Float: return MTL::PixelFormatRGBA32Float;
            
            case DataFormat::D32_Float: return MTL::PixelFormatDepth32Float;
            case DataFormat::D16_UNorm: return MTL::PixelFormatDepth16Unorm;
            case DataFormat::D24_UNorm_S8_UInt: return MTL::PixelFormatDepth24Unorm_Stencil8;
            case DataFormat::D32_Float_S8X24_UInt: return MTL::PixelFormatDepth32Float_Stencil8;
            
            default: return MTL::PixelFormatInvalid;
        }
    }

    MTL::VertexFormat ToMTLVertexFormat(DataFormat format) {
        switch (format) {
            case DataFormat::R8_UNorm: return MTL::VertexFormatUCharNormalized;
            case DataFormat::R8_SNorm: return MTL::VertexFormatCharNormalized;
            case DataFormat::R8_UInt: return MTL::VertexFormatUChar;
            case DataFormat::R8_SInt: return MTL::VertexFormatChar;
            
            case DataFormat::RG8_UNorm: return MTL::VertexFormatUChar2Normalized;
            case DataFormat::RG8_SNorm: return MTL::VertexFormatChar2Normalized;
            case DataFormat::RG8_UInt: return MTL::VertexFormatUChar2;
            case DataFormat::RG8_SInt: return MTL::VertexFormatChar2;

            case DataFormat::R8G8B8_UNorm: return MTL::VertexFormatUChar3Normalized;
            case DataFormat::R8G8B8_SNorm: return MTL::VertexFormatChar3Normalized;
            case DataFormat::R8G8B8_UInt: return MTL::VertexFormatUChar3;
            case DataFormat::R8G8B8_SInt: return MTL::VertexFormatChar3;

            case DataFormat::RGBA8_UNorm: return MTL::VertexFormatUChar4Normalized;
            case DataFormat::RGBA8_SNorm: return MTL::VertexFormatChar4Normalized;
            case DataFormat::RGBA8_UInt: return MTL::VertexFormatUChar4;
            case DataFormat::RGBA8_SInt: return MTL::VertexFormatChar4;

            case DataFormat::R16_UNorm: return MTL::VertexFormatUShortNormalized;
            case DataFormat::R16_SNorm: return MTL::VertexFormatShortNormalized;
            case DataFormat::R16_UInt: return MTL::VertexFormatUShort;
            case DataFormat::R16_SInt: return MTL::VertexFormatShort;
            case DataFormat::R16_Float: return MTL::VertexFormatHalf;

            case DataFormat::RG16_UNorm: return MTL::VertexFormatUShort2Normalized;
            case DataFormat::RG16_SNorm: return MTL::VertexFormatShort2Normalized;
            case DataFormat::RG16_UInt: return MTL::VertexFormatUShort2;
            case DataFormat::RG16_SInt: return MTL::VertexFormatShort2;
            case DataFormat::RG16_Float: return MTL::VertexFormatHalf2;

            case DataFormat::RGBA16_UNorm: return MTL::VertexFormatUShort4Normalized;
            case DataFormat::RGBA16_SNorm: return MTL::VertexFormatShort4Normalized;
            case DataFormat::RGBA16_UInt: return MTL::VertexFormatUShort4;
            case DataFormat::RGBA16_SInt: return MTL::VertexFormatShort4;
            case DataFormat::RGBA16_Float: return MTL::VertexFormatHalf4;

            case DataFormat::R32_UInt: return MTL::VertexFormatUInt;
            case DataFormat::R32_SInt: return MTL::VertexFormatInt;
            case DataFormat::R32_Float: return MTL::VertexFormatFloat;

            case DataFormat::RG32_UInt: return MTL::VertexFormatUInt2;
            case DataFormat::RG32_SInt: return MTL::VertexFormatInt2;
            case DataFormat::RG32_Float: return MTL::VertexFormatFloat2;

            case DataFormat::RGB32_UInt: return MTL::VertexFormatUInt3;
            case DataFormat::RGB32_SInt: return MTL::VertexFormatInt3;
            case DataFormat::RGB32_Float: return MTL::VertexFormatFloat3;

            case DataFormat::RGBA32_UInt: return MTL::VertexFormatUInt4;
            case DataFormat::RGBA32_SInt: return MTL::VertexFormatInt4;
            case DataFormat::RGBA32_Float: return MTL::VertexFormatFloat4;

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

    MTL::StencilOperation ToMTLStencilOperation(StencilOp op) {
        switch (op) {
            case StencilOp::Keep: return MTL::StencilOperationKeep;
            case StencilOp::Zero: return MTL::StencilOperationZero;
            case StencilOp::Replace: return MTL::StencilOperationReplace;
            case StencilOp::IncSat: return MTL::StencilOperationIncrementClamp;
            case StencilOp::DecSat: return MTL::StencilOperationDecrementClamp;
            case StencilOp::Invert: return MTL::StencilOperationInvert;
            case StencilOp::Inc: return MTL::StencilOperationIncrementWrap;
            case StencilOp::Dec: return MTL::StencilOperationDecrementWrap;
            default: return MTL::StencilOperationKeep;
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

bool MetalPipeline::Recreate() {
    // 创建临时副本以避免自我赋值问题（虽然通常是安全的，但为了保险）
    if (isCompute_) {
        ComputePipelineDesc desc = computeDesc_;
        Destroy();
        return Initialize(desc);
    } else {
        GraphicsPipelineDesc desc = graphicsDesc_;
        Destroy();
        return Initialize(desc);
    }
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
    
    if (desc.enableStencilTest) {
        MTL::StencilDescriptor* frontDesc = MTL::StencilDescriptor::alloc()->init();
        frontDesc->setStencilCompareFunction(ToMTLCompareFunction(desc.frontStencil.func));
        frontDesc->setStencilFailureOperation(ToMTLStencilOperation(desc.frontStencil.failOp));
        frontDesc->setDepthFailureOperation(ToMTLStencilOperation(desc.frontStencil.depthFailOp));
        frontDesc->setDepthStencilPassOperation(ToMTLStencilOperation(desc.frontStencil.passOp));
        frontDesc->setReadMask(desc.stencilReadMask);
        frontDesc->setWriteMask(desc.stencilWriteMask);
        depthStencilDesc->setFrontFaceStencil(frontDesc);
        frontDesc->release();

        MTL::StencilDescriptor* backDesc = MTL::StencilDescriptor::alloc()->init();
        backDesc->setStencilCompareFunction(ToMTLCompareFunction(desc.backStencil.func));
        backDesc->setStencilFailureOperation(ToMTLStencilOperation(desc.backStencil.failOp));
        backDesc->setDepthFailureOperation(ToMTLStencilOperation(desc.backStencil.depthFailOp));
        backDesc->setDepthStencilPassOperation(ToMTLStencilOperation(desc.backStencil.passOp));
        backDesc->setReadMask(desc.stencilReadMask);
        backDesc->setWriteMask(desc.stencilWriteMask);
        depthStencilDesc->setBackFaceStencil(backDesc);
        backDesc->release();
    }

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

    // Input Primitive Topology
    MTL::PrimitiveTopologyClass topologyClass = MTL::PrimitiveTopologyClassUnspecified;
    switch (desc.topology) {
        case PrimitiveTopology::PointList: 
            topologyClass = MTL::PrimitiveTopologyClassPoint; 
            break;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip: 
            topologyClass = MTL::PrimitiveTopologyClassLine; 
            break;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip: 
            topologyClass = MTL::PrimitiveTopologyClassTriangle; 
            break;
        default: 
            topologyClass = MTL::PrimitiveTopologyClassUnspecified;
            break;
    }
    pipelineDesc->setInputPrimitiveTopology(topologyClass);

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
