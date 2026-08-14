#include "DawnPipeline.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "DawnDevice.h"
#include "DawnShader.h"
#include "DawnPipelineLayout.h"
#include <iostream>
#include <cstring>
#include <vector>

namespace primal::graphics::rhi {

DawnPipeline::DawnPipeline(DawnDevice& device)
    : device_(device) {
}

DawnPipeline::~DawnPipeline() {
    Destroy();
}

bool DawnPipeline::InitializeGraphics(const GraphicsPipelineDesc& desc) {
    // 1. Get vertex shader module
    DawnShader* vertexShader = device_.GetShader(desc.vertexShader);
    if (!vertexShader || !vertexShader->GetModule()) {
        std::cerr << "[DawnPipeline] Invalid vertex shader handle" << std::endl;
        return false;
    }

    // Get fragment shader module (optional for depth-only passes)
    DawnShader* fragmentShader = nullptr;
    bool hasFragmentShader = (desc.pixelShader != handles::INVALID_SHADER &&
                              desc.pixelShader != static_cast<ShaderHandle>(0));
    if (hasFragmentShader) {
        fragmentShader = device_.GetShader(desc.pixelShader);
        if (!fragmentShader || !fragmentShader->GetModule()) {
            std::cerr << "[DawnPipeline] Invalid fragment shader handle" << std::endl;
            return false;
        }
    }

    // 2. Create WGPUVertexState
    // Map vertex attributes
    std::vector<WGPUVertexAttribute> vertexAttributes(desc.vertexAttributes.size());
    for (u32 i = 0; i < desc.vertexAttributes.size(); ++i) {
        vertexAttributes[i].format = ToWGPUVertexFormat(desc.vertexAttributes[i].format);
        vertexAttributes[i].offset = desc.vertexAttributes[i].offset;
        vertexAttributes[i].shaderLocation = desc.vertexAttributes[i].location;
    }

    // Map vertex bindings
    std::vector<WGPUVertexBufferLayout> vertexBindings(desc.vertexBindings.size());
    for (u32 i = 0; i < desc.vertexBindings.size(); ++i) {
        // Collect attributes for this binding
        std::vector<WGPUVertexAttribute> bindingAttrs;
        for (u32 j = 0; j < desc.vertexAttributes.size(); ++j) {
            if (desc.vertexAttributes[j].binding == desc.vertexBindings[i].binding) {
                bindingAttrs.push_back(vertexAttributes[j]);
            }
        }

        vertexBindings[i].arrayStride = desc.vertexBindings[i].stride;
        vertexBindings[i].stepMode = desc.vertexBindings[i].perVertex
            ? WGPUVertexStepMode_Vertex
            : WGPUVertexStepMode_Instance;
        // We need stable storage for the attribute pointers per binding
        vertexBindings[i].attributeCount = static_cast<u32>(bindingAttrs.size());
        vertexBindings[i].attributes = nullptr; // Will be set below after stable storage
    }

    // Create stable attribute storage per binding (must outlive the descriptor)
    std::vector<std::vector<WGPUVertexAttribute>> stableBindingAttrs(desc.vertexBindings.size());
    for (u32 i = 0; i < desc.vertexBindings.size(); ++i) {
        for (u32 j = 0; j < desc.vertexAttributes.size(); ++j) {
            if (desc.vertexAttributes[j].binding == desc.vertexBindings[i].binding) {
                stableBindingAttrs[i].push_back(vertexAttributes[j]);
            }
        }
        vertexBindings[i].attributes = stableBindingAttrs[i].data();
        vertexBindings[i].attributeCount = static_cast<u32>(stableBindingAttrs[i].size());
    }

    WGPUVertexState vertexState{};
    vertexState.nextInChain = nullptr;
    vertexState.module = vertexShader->GetModule();
    vertexState.entryPoint = ToWGPUStringView(vertexShader->GetEntryPoint());
    vertexState.constantCount = 0;
    vertexState.constants = nullptr;
    vertexState.bufferCount = static_cast<u32>(vertexBindings.size());
    vertexState.buffers = vertexBindings.empty() ? nullptr : vertexBindings.data();

    // 3. Create WGPUPrimitiveState
    WGPUPrimitiveState primitiveState{};
    primitiveState.nextInChain = nullptr;
    primitiveState.topology = ToWGPUPrimitiveTopology(desc.topology);

    // Cull mode mapping
    switch (desc.cullMode) {
        case CullMode::None:  primitiveState.cullMode = WGPUCullMode_None; break;
        case CullMode::Front: primitiveState.cullMode = WGPUCullMode_Front; break;
        case CullMode::Back:  primitiveState.cullMode = WGPUCullMode_Back; break;
        default:              primitiveState.cullMode = WGPUCullMode_None; break;
    }

    primitiveState.frontFace = WGPUFrontFace_CCW;

    // Strip index format for strip topologies
    if (desc.topology == PrimitiveTopology::TriangleStrip ||
        desc.topology == PrimitiveTopology::LineStrip) {
        primitiveState.stripIndexFormat = WGPUIndexFormat_Uint32;
    } else {
        primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;
    }

    // 4. Create WGPUFragmentState with blend and render target formats
    std::vector<WGPUColorTargetState> colorTargets(desc.renderTargetCount);
    std::vector<WGPUBlendState> blendStates(desc.renderTargetCount);

    for (u32 i = 0; i < desc.renderTargetCount; ++i) {
        WGPUColorTargetState& target = colorTargets[i];
        target.nextInChain = nullptr;
        target.format = ToWGPUTextureFormat(desc.renderTargetFormats[i]);

        // Blend state per render target
        WGPUBlendState& blend = blendStates[i];
        if (desc.enableBlend) {
            blend.color.operation = ToWGPUBlendOperation(desc.colorBlendOp);
            blend.color.srcFactor = ToWGPUBlendFactor(desc.srcColorBlendFactor);
            blend.color.dstFactor = ToWGPUBlendFactor(desc.dstColorBlendFactor);
            blend.alpha.operation = ToWGPUBlendOperation(desc.alphaBlendOp);
            blend.alpha.srcFactor = ToWGPUBlendFactor(desc.srcAlphaBlendFactor);
            blend.alpha.dstFactor = ToWGPUBlendFactor(desc.dstAlphaBlendFactor);
        } else {
            blend.color.operation = WGPUBlendOperation_Add;
            blend.color.srcFactor = WGPUBlendFactor_One;
            blend.color.dstFactor = WGPUBlendFactor_Zero;
            blend.alpha.operation = WGPUBlendOperation_Add;
            blend.alpha.srcFactor = WGPUBlendFactor_One;
            blend.alpha.dstFactor = WGPUBlendFactor_Zero;
        }

        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;
    }

    WGPUFragmentState fragmentState{};
    if (hasFragmentShader) {
        fragmentState.nextInChain = nullptr;
        fragmentState.module = fragmentShader->GetModule();
        fragmentState.entryPoint = ToWGPUStringView(fragmentShader->GetEntryPoint());
        fragmentState.constantCount = 0;
        fragmentState.constants = nullptr;
        fragmentState.targetCount = static_cast<u32>(colorTargets.size());
        fragmentState.targets = colorTargets.empty() ? nullptr : colorTargets.data();
    }

    // 5. Create WGPUDepthStencilState
    WGPUDepthStencilState depthStencilState{};
    bool hasDepthStencil = (desc.depthStencilFormat != DataFormat::Unknown);

    if (hasDepthStencil) {
        depthStencilState.nextInChain = nullptr;
        depthStencilState.format = ToWGPUTextureFormat(desc.depthStencilFormat);
        depthStencilState.depthWriteEnabled = desc.enableDepthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depthStencilState.depthCompare = desc.enableDepthTest
            ? ToWGPUCompareFunction(desc.depthFunc)
            : WGPUCompareFunction_Always;

        depthStencilState.stencilFront.compare = ToWGPUCompareFunction(desc.frontStencil.func);
        depthStencilState.stencilFront.failOp = ToWGPUStencilOperation(desc.frontStencil.failOp);
        depthStencilState.stencilFront.depthFailOp = ToWGPUStencilOperation(desc.frontStencil.depthFailOp);
        depthStencilState.stencilFront.passOp = ToWGPUStencilOperation(desc.frontStencil.passOp);

        depthStencilState.stencilBack.compare = ToWGPUCompareFunction(desc.backStencil.func);
        depthStencilState.stencilBack.failOp = ToWGPUStencilOperation(desc.backStencil.failOp);
        depthStencilState.stencilBack.depthFailOp = ToWGPUStencilOperation(desc.backStencil.depthFailOp);
        depthStencilState.stencilBack.passOp = ToWGPUStencilOperation(desc.backStencil.passOp);

        depthStencilState.stencilReadMask = desc.stencilReadMask;
        depthStencilState.stencilWriteMask = desc.stencilWriteMask;
        depthStencilState.depthBias = static_cast<s32>(desc.depthBias);
        depthStencilState.depthBiasClamp = desc.depthBiasClamp;
        depthStencilState.depthBiasSlopeScale = desc.slopeScaledDepthBias;
    }

    // 6. Create WGPUMultisampleState
    WGPUMultisampleState multisampleState{};
    multisampleState.nextInChain = nullptr;
    multisampleState.count = 1; // Default to 1 sample; could be derived from render target
    multisampleState.mask = 0xFFFFFFFF;
    multisampleState.alphaToCoverageEnabled = false;

    // 7. Get pipeline layout
    WGPUPipelineLayout wgpuLayout = nullptr;
    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        DawnPipelineLayout* layout = device_.GetPipelineLayout(desc.layout);
        if (layout) {
            wgpuLayout = layout->GetNativeLayout();
        }
    }
    layout_ = desc.layout;

    // 8. Create WGPURenderPipelineDescriptor
    WGPURenderPipelineDescriptor pipelineDesc{};
    pipelineDesc.nextInChain = nullptr;
    pipelineDesc.label = ToWGPUStringView("DawnGraphicsPipeline");
    pipelineDesc.layout = wgpuLayout;
    pipelineDesc.vertex = vertexState;
    pipelineDesc.primitive = primitiveState;
    pipelineDesc.depthStencil = hasDepthStencil ? &depthStencilState : nullptr;
    pipelineDesc.multisample = multisampleState;
    pipelineDesc.fragment = hasFragmentShader ? &fragmentState : nullptr;

    wgpuRenderPipeline_ = wgpuDeviceCreateRenderPipeline(device_.GetNativeDevice(), &pipelineDesc);
    if (!wgpuRenderPipeline_) {
        std::cerr << "[DawnPipeline] Failed to create graphics pipeline" << std::endl;
        return false;
    }

    isCompute_ = false;
    return true;
}

bool DawnPipeline::InitializeCompute(const ComputePipelineDesc& desc) {
    // 1. Get compute shader module
    DawnShader* computeShader = device_.GetShader(desc.computeShader);
    if (!computeShader || !computeShader->GetModule()) {
        std::cerr << "[DawnPipeline] Invalid compute shader handle" << std::endl;
        return false;
    }

    // 2. Get pipeline layout
    WGPUPipelineLayout wgpuLayout = nullptr;
    if (desc.layout != handles::INVALID_PIPELINE_LAYOUT) {
        DawnPipelineLayout* layout = device_.GetPipelineLayout(desc.layout);
        if (layout) {
            wgpuLayout = layout->GetNativeLayout();
        }
    }
    layout_ = desc.layout;

    // 3. Create compute pipeline
    std::string pipelineLabel = std::string("Compute_") + computeShader->GetEntryPoint();
    WGPUComputePipelineDescriptor pipelineDesc{};
    pipelineDesc.nextInChain = nullptr;
    pipelineDesc.label = ToWGPUStringView(pipelineLabel.c_str());
    pipelineDesc.layout = wgpuLayout;
    pipelineDesc.compute.nextInChain = nullptr;
    pipelineDesc.compute.module = computeShader->GetModule();
    pipelineDesc.compute.entryPoint = ToWGPUStringView(computeShader->GetEntryPoint());
    pipelineDesc.compute.constantCount = 0;
    pipelineDesc.compute.constants = nullptr;

    wgpuComputePipeline_ = wgpuDeviceCreateComputePipeline(device_.GetNativeDevice(), &pipelineDesc);
    if (!wgpuComputePipeline_) {
        std::cerr << "[DawnPipeline] Failed to create compute pipeline: " << pipelineLabel << std::endl;
        return false;
    }

    isCompute_ = true;
    return true;
}

void DawnPipeline::Destroy() {
    if (wgpuRenderPipeline_) {
        wgpuRenderPipelineRelease(wgpuRenderPipeline_);
        wgpuRenderPipeline_ = nullptr;
    }
    if (wgpuComputePipeline_) {
        wgpuComputePipelineRelease(wgpuComputePipeline_);
        wgpuComputePipeline_ = nullptr;
    }
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
