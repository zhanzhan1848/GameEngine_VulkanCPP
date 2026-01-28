#include "TAAPass.h"
#include <fstream>
#include <iostream>
#include <vector>

namespace primal::graphics {

TAAPass::TAAPass() {
    for (uint32_t i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptorSets_[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
        uniformBuffers_[i] = rhi::handles::INVALID_RESOURCE;
        uniformBuffersMapped_[i] = nullptr;
    }
}

TAAPass::~TAAPass() {
    Shutdown();
}

bool TAAPass::Initialize(rhi::RHIDeviceBase* device, uint32_t width, uint32_t height, rhi::DataFormat outputFormat) {
    device_ = device;
    if (!device_) return false;

    // 1. Create Descriptor Set Layout
    std::vector<rhi::DescriptorSetLayoutBinding> bindings;
    // Binding 0: Color Texture
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 0;
        b.descriptorType = rhi::DescriptorType::SampledImage;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Pixel;
        bindings.push_back(b);
    }
    // Binding 1: History Texture
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 1;
        b.descriptorType = rhi::DescriptorType::SampledImage;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Pixel;
        bindings.push_back(b);
    }
    // Binding 2: Velocity Texture
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 2;
        b.descriptorType = rhi::DescriptorType::SampledImage;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Pixel;
        bindings.push_back(b);
    }
    // Binding 3: Uniforms
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = 3;
        b.descriptorType = rhi::DescriptorType::UniformBuffer;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Pixel;
        bindings.push_back(b);
    }
    
    rhi::DescriptorSetLayoutDesc dslDesc;
    dslDesc.bindings = bindings.data();
    dslDesc.bindingCount = (uint32_t)bindings.size();
    
    descriptorSetLayout_ = device_->CreateDescriptorSetLayout(dslDesc);

    // 2. Create Pipeline Layout
    rhi::PipelineLayoutDesc plDesc;
    plDesc.setLayouts = &descriptorSetLayout_;
    plDesc.setLayoutCount = 1;
    pipelineLayout_ = device_->CreatePipelineLayout(plDesc);

    // 3. Load Shaders
    auto loadShader = [&](const std::string& path, rhi::ShaderStage stage, const char* entry) -> rhi::ShaderHandle {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
             std::string absPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + path;
             file.open(absPath, std::ios::ate | std::ios::binary);
        }
        if (!file.is_open()) return rhi::handles::INVALID_SHADER;
        
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize + 1);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        buffer[fileSize] = '\0';
        
        return device_->CreateShader(buffer.data(), fileSize + 1, stage, entry);
    };

    vertexShader_ = loadShader("EngineTest/shaders/TAA.metal", rhi::ShaderStage::Vertex, "vertexMain");
    fragmentShader_ = loadShader("EngineTest/shaders/TAA.metal", rhi::ShaderStage::Pixel, "fragmentMain");

    if (vertexShader_ == rhi::handles::INVALID_SHADER || fragmentShader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "TAAPass: Failed to load shaders." << std::endl;
        return false;
    }

    // 4. Create Pipeline
    rhi::GraphicsPipelineDesc gpDesc;
    gpDesc.vertexShader = vertexShader_;
    gpDesc.pixelShader = fragmentShader_;
    gpDesc.layout = pipelineLayout_;
    
    gpDesc.topology = rhi::PrimitiveTopology::TriangleList;
    
    gpDesc.cullMode = rhi::CullMode::None;
    gpDesc.fillMode = rhi::FillMode::Solid;
    
    gpDesc.renderTargetCount = 1;
    gpDesc.renderTargetFormats[0] = outputFormat;
    
    gpDesc.depthStencilFormat = rhi::DataFormat::Unknown;

    pipeline_ = device_->CreateGraphicsPipeline(gpDesc);

    // 5. Create Per-Frame Resources
    for (uint32_t i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetDesc desc;
        desc.layout = descriptorSetLayout_;
        descriptorSets_[i] = device_->CreateDescriptorSet(desc);
        
        rhi::BufferDesc bufDesc;
        bufDesc.size = sizeof(TAAUniforms);
        bufDesc.usage = rhi::GPUMemoryUsage::Dynamic;
        bufDesc.type = rhi::BufferType::Constant;
        bufDesc.bindFlags = (uint32_t)rhi::BufferUsageFlags::Uniform;
        uniformBuffers_[i] = device_->CreateBuffer(bufDesc);
        
        uniformBuffersMapped_[i] = device_->MapBuffer(uniformBuffers_[i]);
    }

    return true;
}

void TAAPass::Shutdown() {
    // Basic cleanup - in a real engine we'd destroy resources
}

void TAAPass::Execute(rhi::RHICommandBuffer* cmdBuffer,
                      rhi::ResourceHandle colorInput,
                      rhi::ResourceHandle historyInput,
                      rhi::ResourceHandle velocityInput,
                      rhi::ResourceHandle output,
                      uint32_t width, uint32_t height,
                      uint32_t frameIndex,
                      float jitterX, float jitterY,
                      float prevJitterX, float prevJitterY) {
    
    // Debug: Verify dimensions
    if (width == 0 || height == 0) {
        std::cout << "TAAPass::Execute - Invalid Dimensions: " << width << "x" << height << std::endl;
        return;
    }
    
    // Update Uniforms
    TAAUniforms uniforms;
    uniforms.resolution[0] = (float)width;
    uniforms.resolution[1] = (float)height;
    uniforms.jitter[0] = jitterX;
    uniforms.jitter[1] = jitterY;
    uniforms.previousJitter[0] = prevJitterX;
    uniforms.previousJitter[1] = prevJitterY;
    uniforms.feedback = 0.95f;
    uniforms.padding = 0.0f;
    
    if (uniformBuffersMapped_[frameIndex]) {
        memcpy(uniformBuffersMapped_[frameIndex], &uniforms, sizeof(TAAUniforms));
    }
    
    // Update Descriptor Set
    std::vector<rhi::WriteDescriptorSet> writes;
    
    // 0: Color
    rhi::DescriptorImageInfo colorInfo;
    colorInfo.imageView = colorInput;
    colorInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet colorWrite;
    colorWrite.dstSet = descriptorSets_[frameIndex];
    colorWrite.dstBinding = 0;
    colorWrite.descriptorCount = 1;
    colorWrite.descriptorType = rhi::DescriptorType::SampledImage;
    colorWrite.imageInfo = &colorInfo;
    writes.push_back(colorWrite);
    
    // 1: History
    rhi::DescriptorImageInfo historyInfo;
    historyInfo.imageView = historyInput;
    historyInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet historyWrite;
    historyWrite.dstSet = descriptorSets_[frameIndex];
    historyWrite.dstBinding = 1;
    historyWrite.descriptorCount = 1;
    historyWrite.descriptorType = rhi::DescriptorType::SampledImage;
    historyWrite.imageInfo = &historyInfo;
    writes.push_back(historyWrite);
    
    // 2: Velocity
    rhi::DescriptorImageInfo velocityInfo;
    velocityInfo.imageView = velocityInput;
    velocityInfo.imageLayout = rhi::ResourceState::ShaderResource;
    
    rhi::WriteDescriptorSet velocityWrite;
    velocityWrite.dstSet = descriptorSets_[frameIndex];
    velocityWrite.dstBinding = 2;
    velocityWrite.descriptorCount = 1;
    velocityWrite.descriptorType = rhi::DescriptorType::SampledImage;
    velocityWrite.imageInfo = &velocityInfo;
    writes.push_back(velocityWrite);
    
    // 3: Uniforms
    rhi::DescriptorBufferInfo bufferInfo;
    bufferInfo.buffer = uniformBuffers_[frameIndex];
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(TAAUniforms);
    
    rhi::WriteDescriptorSet bufferWrite;
    bufferWrite.dstSet = descriptorSets_[frameIndex];
    bufferWrite.dstBinding = 3;
    bufferWrite.descriptorCount = 1;
    bufferWrite.descriptorType = rhi::DescriptorType::UniformBuffer;
    bufferWrite.bufferInfo = &bufferInfo;
    writes.push_back(bufferWrite);
    
    device_->UpdateDescriptorSets((uint32_t)writes.size(), writes.data());
    
    
    // Begin Render Pass - REMOVED (RenderGraph handles this)
    /*
    rhi::RenderPassBeginInfo beginInfo;
    beginInfo.colorAttachments[0].texture = output;
    beginInfo.colorAttachments[0].loadOp = rhi::LoadOp::DontCare;
    beginInfo.colorAttachments[0].storeOp = rhi::StoreOp::Store;
    beginInfo.colorAttachments[0].clearColor = {0,0,0,0};
    beginInfo.colorAttachmentCount = 1;
    
    cmdBuffer->BeginRenderPass(beginInfo);
    */
    
    cmdBuffer->BindGraphicsPipeline(pipeline_);
    
    rhi::ViewportDesc vp;
    vp.topLeft = {0, 0};
    vp.size = {(float)width, (float)height};
    vp.minDepth = 0; vp.maxDepth = 1;
    cmdBuffer->SetViewport(vp);
    
    rhi::Rect scissor;
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    cmdBuffer->SetScissor(scissor);
    
    const rhi::DescriptorSetHandle sets[] = { descriptorSets_[frameIndex] };
    cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipelineLayout_, 0, 1, sets, 0, nullptr);
    
    // Draw(vertexCount, startVertex, instanceCount, startInstance)
    cmdBuffer->Draw(3, 0, 1, 0);
    
    // cmdBuffer->EndRenderPass();
}

} // namespace primal::graphics
