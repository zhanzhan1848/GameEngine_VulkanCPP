#include "Material.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <cstring>

namespace primal::graphics {

Material::Material() = default;

Material::~Material() {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    InvalidatePipelinesLocked();
    
    // Destroy shaders
    if (device_) {
        for (auto& [permId, stageMap] : shaderVariants_) {
            for (auto& [stage, entry] : stageMap) {
                if (entry.handle != rhi::handles::INVALID_SHADER) {
                    device_->DestroyShader(entry.handle);
                }
            }
        }
    }
}

void Material::SetShader(rhi::ShaderStage stage, const void* data, u64 size, const char* entryPoint, u32 permutationId) {
    ShaderBytecode bytecode(size);
    if (size > 0) {
        std::memcpy(bytecode.data(), data, size);
    }
    SetShader(stage, bytecode, entryPoint, permutationId);
}

void Material::SetShader(rhi::ShaderStage stage, const ShaderBytecode& bytecode, const char* entryPoint, u32 permutationId) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    
    auto& entry = shaderVariants_[permutationId][stage];
    entry.bytecode = bytecode;
    entry.entryPoint = entryPoint ? entryPoint : "main";
    
    if (entry.handle != rhi::handles::INVALID_SHADER && device_) {
        device_->DestroyShader(entry.handle);
        entry.handle = rhi::handles::INVALID_SHADER;
    }
    
    InvalidatePipelinesLocked();
}

void Material::SetVertexAttributes(const utl::vector<rhi::VertexInputAttribute>& attributes) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    vertexAttributes_ = attributes;
    InvalidatePipelinesLocked();
}

void Material::SetVertexBindings(const utl::vector<rhi::VertexInputBinding>& bindings) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    vertexBindings_ = bindings;
    InvalidatePipelinesLocked();
}

void Material::SetBlendState(const rhi::BlendState& state) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    blendState_ = state;
    InvalidatePipelinesLocked();
}

void Material::SetDepthStencilState(const rhi::DepthStencilState& state) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    depthStencilState_ = state;
    InvalidatePipelinesLocked();
}

void Material::SetRasterizerState(const rhi::RasterizerState& state) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    rasterizerState_ = state;
    InvalidatePipelinesLocked();
}

void Material::SetTopology(rhi::PrimitiveTopology topology) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    topology_ = topology;
    InvalidatePipelinesLocked();
}

void Material::SetPipelineLayout(rhi::PipelineLayoutHandle layout) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    layout_ = layout;
    InvalidatePipelinesLocked();
}

void Material::SetDescriptorSetLayout(rhi::DescriptorSetLayoutHandle layout) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    descriptorSetLayout_ = layout;
    InvalidatePipelinesLocked();
}

void Material::SetUniformBlockSize(u32 size) {
    uniformBlockSize_ = size;
}

void Material::SetUniformBufferBinding(u32 binding) {
    uniformBufferBinding_ = binding;
}

void Material::SetRenderTargetFormats(const utl::vector<rhi::DataFormat>& formats, rhi::DataFormat depthStencilFormat) {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    renderTargetFormats_ = formats;
    depthStencilFormat_ = depthStencilFormat;
    InvalidatePipelinesLocked();
}

rhi::PipelineHandle Material::GetPipeline(rhi::RHIDeviceBase* device, rhi::RenderPassHandle renderPass, u32 permutationId, PipelineFlags flags) {
    if (!device) return rhi::handles::INVALID_PIPELINE;

    std::lock_guard<std::mutex> lock(pipelineMutex_);
    device_ = device;

    // 1. Ensure Shaders are created for this permutation
    auto& stageMap = shaderVariants_[permutationId];
    for (auto& [stage, entry] : stageMap) {
        if (entry.handle == rhi::handles::INVALID_SHADER && !entry.bytecode.empty()) {
            entry.handle = device->CreateShader(
                entry.bytecode.data(), 
                entry.bytecode.size(), 
                stage, 
                entry.entryPoint.c_str()
            );
        }
    }

    // 2. Check Cache
    PipelineKey key{renderPass, permutationId, flags};
    if (auto it = pipelineCache_.find(key); it != pipelineCache_.end()) {
        return it->second;
    }

    // 3. Create Pipeline
    rhi::GraphicsPipelineDesc desc;
    
    if (stageMap.count(rhi::ShaderStage::Vertex)) desc.vertexShader = stageMap[rhi::ShaderStage::Vertex].handle;
    
    bool isDepthOnly = (flags & PipelineFlags::DepthOnly) != PipelineFlags::None;
    bool isDepthEqual = (flags & PipelineFlags::DepthEqual) != PipelineFlags::None;

    if (!isDepthOnly && stageMap.count(rhi::ShaderStage::Pixel)) desc.pixelShader = stageMap[rhi::ShaderStage::Pixel].handle;
    if (stageMap.count(rhi::ShaderStage::Geometry)) desc.geometryShader = stageMap[rhi::ShaderStage::Geometry].handle;
    if (stageMap.count(rhi::ShaderStage::Hull)) desc.hullShader = stageMap[rhi::ShaderStage::Hull].handle;
    if (stageMap.count(rhi::ShaderStage::Domain)) desc.domainShader = stageMap[rhi::ShaderStage::Domain].handle;

    desc.layout = layout_;
    desc.vertexAttributes = vertexAttributes_;
    desc.vertexBindings = vertexBindings_;
    desc.topology = topology_;

    desc.fillMode = rasterizerState_.fillMode;
    desc.cullMode = rasterizerState_.cullMode;
    
    desc.enableDepthTest = depthStencilState_.enableDepthTest;
    desc.enableDepthWrite = depthStencilState_.enableDepthWrite;
    desc.depthFunc = depthStencilState_.depthFunc;
    desc.enableStencilTest = depthStencilState_.enableStencilTest;
    desc.stencilReadMask = depthStencilState_.stencilReadMask;
    desc.stencilWriteMask = depthStencilState_.stencilWriteMask;
    desc.frontStencil = depthStencilState_.frontStencil;
    desc.backStencil = depthStencilState_.backStencil;
    
    if (isDepthOnly) {
        desc.enableDepthWrite = true;
        desc.depthFunc = rhi::ComparisonFunc::Less;
        desc.renderTargetCount = 0;
        desc.enableBlend = false;
    } else if (isDepthEqual) {
        desc.enableDepthWrite = false;
        desc.depthFunc = rhi::ComparisonFunc::Equal;
        desc.enableBlend = blendState_.enableBlend;
    } else {
        desc.enableBlend = blendState_.enableBlend;
    }

    desc.srcColorBlendFactor = blendState_.srcColorBlendFactor;
    desc.dstColorBlendFactor = blendState_.dstColorBlendFactor;
    desc.colorBlendOp = blendState_.colorBlendOp;
    desc.srcAlphaBlendFactor = blendState_.srcAlphaBlendFactor;
    desc.dstAlphaBlendFactor = blendState_.dstAlphaBlendFactor;
    desc.alphaBlendOp = blendState_.alphaBlendOp;
    desc.blendConstants = blendState_.blendConstants;
    
    if (!isDepthOnly) {
        desc.renderTargetCount = static_cast<uint32_t>(renderTargetFormats_.size());
        for (size_t i = 0; i < renderTargetFormats_.size() && i < rhi::constants::MAX_RENDER_TARGETS; ++i) {
            desc.renderTargetFormats[i] = renderTargetFormats_[i];
        }
    }
    desc.depthStencilFormat = depthStencilFormat_;

    rhi::PipelineHandle pipeline = device->CreateGraphicsPipeline(desc);
    if (pipeline != rhi::handles::INVALID_PIPELINE) {
        pipelineCache_[key] = pipeline;
    }
    
    return pipeline;
}

void Material::InvalidatePipelines() {
    std::lock_guard<std::mutex> lock(pipelineMutex_);
    InvalidatePipelinesLocked();
}

void Material::InvalidatePipelinesLocked() {
    if (device_) {
        for (auto& [key, pipeline] : pipelineCache_) {
            device_->DestroyPipeline(pipeline);
        }
    }
    pipelineCache_.clear();
}

} // namespace primal::graphics
