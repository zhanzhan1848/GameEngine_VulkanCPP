#include "DepthHistoryManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include <iostream>
#include <fstream>

namespace primal::graphics::nanite {

DepthHistoryManager::~DepthHistoryManager() {
    Shutdown();
}

bool DepthHistoryManager::Initialize(rhi::RHIDeviceBase* device, const Config& config) {
    if (initialized_) {
        std::cerr << "[DepthHistoryManager] Already initialized" << std::endl;
        return false;
    }

    if (!device) {
        std::cerr << "[DepthHistoryManager] Invalid device pointer" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;

    if (config_.buffer_count < 2 || config_.buffer_count > 4) {
        std::cerr << "[DepthHistoryManager] Invalid buffer count: " << config_.buffer_count
                  << " (must be 2-4)" << std::endl;
        return false;
    }

    if (!CreateDepthResources()) {
        std::cerr << "[DepthHistoryManager] Failed to create depth resources" << std::endl;
        return false;
    }

    if (!CreateCopyPipeline()) {
        std::cerr << "[DepthHistoryManager] Failed to create depth copy pipeline" << std::endl;
        return false;
    }

    initialized_ = true;
    return true;
}

void DepthHistoryManager::Shutdown() {
    if (!initialized_) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (device_) {
        for (auto& depth_buffer : depth_buffers_) {
            depth_buffer.texture = rhi::handles::INVALID_RESOURCE;
            depth_buffer.frame_index = 0xFFFFFFFF;
            depth_buffer.is_valid = false;
        }

        if (depth_copy_pipeline_ != rhi::handles::INVALID_PIPELINE)
            device_->DestroyPipeline(depth_copy_pipeline_);
        if (depth_copy_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT)
            device_->DestroyPipelineLayout(depth_copy_layout_);
        if (depth_copy_ds_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)
            device_->DestroyDescriptorSetLayout(depth_copy_ds_layout_);
        if (depth_copy_ds_ != rhi::handles::INVALID_DESCRIPTOR_SET)
            device_->DestroyDescriptorSet(depth_copy_ds_);
        if (depth_copy_cb_ != rhi::handles::INVALID_RESOURCE)
            device_->DestroyBuffer(depth_copy_cb_);

        depth_copy_pipeline_ = rhi::handles::INVALID_PIPELINE;
        depth_copy_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
        depth_copy_ds_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        depth_copy_ds_ = rhi::handles::INVALID_DESCRIPTOR_SET;
        depth_copy_cb_ = rhi::handles::INVALID_RESOURCE;
    }

    initialized_ = false;
    current_write_index_ = 0;
}

bool DepthHistoryManager::CreateDepthResources() {
//    std::cout << "[DepthHistoryManager] Creating depth resources..." << std::endl;

    // Create depth textures for each buffer
    for (size_t i = 0; i < config_.buffer_count; ++i) {
        rhi::TextureDesc depthDesc{};
        depthDesc.size = { config_.width, config_.height, 1 };
        depthDesc.format = config_.format;
        depthDesc.type = rhi::TextureType::Texture2D;
        depthDesc.mipLevels = 1; // Single mip level for depth history
        depthDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest | rhi::TextureUsage::UnorderedAccess;

        depth_buffers_[i].texture = device_->CreateTexture(depthDesc);
        if (depth_buffers_[i].texture == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[DepthHistoryManager] Failed to create depth texture " << i << std::endl;
            return false;
        }

        depth_buffers_[i].frame_index = 0xFFFFFFFF;
        depth_buffers_[i].is_valid = false;

//        std::cout << "  Created depth buffer " << i << std::endl;
    }

    return true;
}

bool DepthHistoryManager::StoreCurrentFrameDepth(rhi::ResourceHandle depth_texture,
                                                rhi::RHICommandBuffer* cmd_buffer,
                                                u32 frame_index) {
    if (!initialized_ || !cmd_buffer) {
        std::cerr << "[DepthHistoryManager] Not initialized or invalid command buffer" << std::endl;
        return false;
    }

    if (depth_texture == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[DepthHistoryManager] Invalid source depth texture" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Determine which buffer to write to (round-robin through buffers)
    u32 write_index = frame_index % config_.buffer_count;

    // Copy current depth to history buffer
    if (!CopyDepthTexture(depth_texture, depth_buffers_[write_index].texture, cmd_buffer)) {
        std::cerr << "[DepthHistoryManager] Failed to copy depth texture to buffer " << write_index << std::endl;
        return false;
    }

    // Update buffer metadata
    depth_buffers_[write_index].frame_index = frame_index;
    depth_buffers_[write_index].is_valid = true;
    current_write_index_ = write_index;

//    std::cout << "[DepthHistoryManager] Stored frame " << frame_index
//              << " depth in buffer " << write_index << std::endl;

    return true;
}

DepthHistoryManager::DepthBuffer DepthHistoryManager::GetPreviousFrameDepth(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[DepthHistoryManager] Not initialized" << std::endl;
        return {};
    }

    // Calculate which buffer contains the previous frame
    // If current frame is N, we want frame N-1
    u32 prev_frame_index = (frame_index == 0) ? 0 : frame_index - 1;
    u32 prev_buffer_index = prev_frame_index % config_.buffer_count;

    const auto& buffer = depth_buffers_[prev_buffer_index];

    // Validate that this buffer actually contains the previous frame
    if (buffer.frame_index != prev_frame_index || !buffer.is_valid) {
//        std::cout << "[DepthHistoryManager] Previous frame depth not available (frame "
//                  << prev_frame_index << " in buffer " << prev_buffer_index << ")" << std::endl;
        return {}; // Return invalid buffer
    }

//    std::cout << "[DepthHistoryManager] Retrieved previous frame " << prev_frame_index
//              << " from buffer " << prev_buffer_index << std::endl;

    return buffer;
}

DepthHistoryManager::DepthBuffer DepthHistoryManager::GetCurrentFrameDepth(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[DepthHistoryManager] Not initialized" << std::endl;
        return {};
    }

    u32 current_buffer_index = frame_index % config_.buffer_count;
    return depth_buffers_[current_buffer_index];
}

bool DepthHistoryManager::IsPreviousFrameDepthAvailable(u32 frame_index) const {
    auto prev_depth = GetPreviousFrameDepth(frame_index);
    return prev_depth.is_valid && prev_depth.texture != rhi::handles::INVALID_RESOURCE;
}

bool DepthHistoryManager::CopyDepthTexture(rhi::ResourceHandle source,
                                          rhi::ResourceHandle destination,
                                          rhi::RHICommandBuffer* cmd_buffer) {
    if (!device_ || !cmd_buffer) return false;
    if (source == rhi::handles::INVALID_RESOURCE || destination == rhi::handles::INVALID_RESOURCE) return false;
    if (depth_copy_pipeline_ == rhi::handles::INVALID_PIPELINE) return false;

    // Update descriptor set: source = SampledImage, destination = StorageImage
    rhi::DescriptorImageInfo srcInfo{ rhi::handles::INVALID_SAMPLER, source, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo dstInfo{ rhi::handles::INVALID_SAMPLER, destination, rhi::ResourceState::UnorderedAccess };

    rhi::WriteDescriptorSet writes[2];
    writes[0] = { depth_copy_ds_, 0, 0, 1, rhi::DescriptorType::SampledImage, &srcInfo, nullptr };
    writes[1] = { depth_copy_ds_, 1, 0, 1, rhi::DescriptorType::StorageImage, &dstInfo, nullptr };

    device_->UpdateDescriptorSets(2, writes);

    u32 groupsX = (config_.width + 15) / 16;
    u32 groupsY = (config_.height + 15) / 16;

    cmd_buffer->BindComputePipeline(depth_copy_pipeline_);
    rhi::DescriptorSetHandle dsHandle = depth_copy_ds_;
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, depth_copy_layout_, 0, 1, &dsHandle, 0, nullptr);
    cmd_buffer->Dispatch(groupsX, groupsY, 1);
    cmd_buffer->MemoryBarrier(
        rhi::PipelineStage::ComputeShader,
        rhi::PipelineStage::ComputeShader,
        rhi::AccessFlag::ShaderWrite,
        rhi::AccessFlag::ShaderRead
    );

    return true;
}

bool DepthHistoryManager::CreateCopyPipeline() {
    // T4.6.5 part 30.10 (Bug G): Vulkan skip. Hardcoded Metal shader path
    // (HZBAnimation.metal, 10941 bytes non-multiple-of-4) is rejected by
    // VulkanShader which requires SPIR-V binary (size % 4 == 0). Depth history
    // is only consumed by temporal effects (TAA, motion blur) — both disabled
    // on Vulkan per current scope. Early-return leaves depth_copy_pipeline_
    // INVALID, and Capture() guards with that check (line 193).
    if (device_ && device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        std::cerr << "[DepthHistoryManager] Skipped on Vulkan (Metal-only depth copy shader; "
                     "TAA/motion-blur not yet active on this backend)" << std::endl;
        return true;
    }

    // Descriptor layout: texture(0) = SampledImage (D32 depth), texture(1) = StorageImage (R32 history)
    rhi::DescriptorSetLayoutBinding bindings[] = {
        { 0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute, nullptr },
        { 1, rhi::DescriptorType::StorageImage, 1, rhi::ShaderStage::Compute, nullptr }
    };

    rhi::DescriptorSetLayoutDesc layoutDesc{ 2, bindings };
    depth_copy_ds_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    if (depth_copy_ds_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

    rhi::PipelineLayoutDesc pipelineLayoutDesc{ 1, &depth_copy_ds_layout_, 0, nullptr };
    depth_copy_layout_ = device_->CreatePipelineLayout(pipelineLayoutDesc);
    if (depth_copy_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;

    // Load HZBGeneration shader (contains copy_depth_to_hzb_mip0 entry point)
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/HZBGeneration.metal";
    std::ifstream file(shaderPath);
    if (!file.is_open()) {
        std::cerr << "[DepthHistoryManager] Failed to open shader: " << shaderPath << std::endl;
        return false;
    }
    std::string code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    auto shader = device_->CreateShader(code.data(), code.size(), rhi::ShaderStage::Compute, "copy_depth_to_hzb_mip0");
    if (shader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[DepthHistoryManager] Failed to create depth copy shader" << std::endl;
        return false;
    }

    rhi::ComputePipelineDesc computeDesc{};
    computeDesc.computeShader = shader;
    computeDesc.layout = depth_copy_layout_;
    depth_copy_pipeline_ = device_->CreateComputePipeline(computeDesc);
    if (depth_copy_pipeline_ == rhi::handles::INVALID_PIPELINE) return false;

    // Create one descriptor set (reused for all copies — updated before each dispatch)
    rhi::DescriptorSetDesc dsDesc{ depth_copy_ds_layout_ };
    depth_copy_ds_ = device_->CreateDescriptorSet(dsDesc);
    if (depth_copy_ds_ == rhi::handles::INVALID_DESCRIPTOR_SET) return false;

    return true;
}

} // namespace primal::graphics::nanite