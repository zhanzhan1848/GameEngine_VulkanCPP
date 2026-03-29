#include "HZBSystem.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

namespace primal::graphics::nanite {

HZBSystem::~HZBSystem() {
    Shutdown();
}

bool HZBSystem::Initialize(rhi::RHIDeviceBase* device, const Config& config) {
    if (initialized_) {
        std::cerr << "[HZBSystem] Already initialized" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;

    std::cout << "[HZBSystem] Initializing..." << std::endl;
    std::cout << "  Max Resolution: " << config_.max_width << "x" << config_.max_height << std::endl;
    std::cout << "  Min Mip Size: " << config_.min_mip_size << "x" << config_.min_mip_size << std::endl;
    std::cout << "  GPU Generation: " << (config_.generate_on_gpu ? "Enabled" : "Disabled") << std::endl;

    if (!CreateHZBResources()) {
        std::cerr << "[HZBSystem] Failed to create HZB resources" << std::endl;
        return false;
    }

    if (!CreateHZBSampler()) {
        std::cerr << "[HZBSystem] Failed to create HZB sampler" << std::endl;
        return false;
    }

    if (config_.generate_on_gpu && !CreateHZBComputePipeline()) {
        std::cout << "[HZBSystem] GPU pipeline creation failed, falling back to CPU" << std::endl;
        config_.generate_on_gpu = false;
    }

    initialized_ = true;
    std::cout << "[HZBSystem] Initialized successfully" << std::endl;
    std::cout << "  Mip Levels: " << mip_levels_ << std::endl;

    return true;
}

void HZBSystem::Shutdown() {
    if (!initialized_) return;

    std::cout << "[HZBSystem] Shutting down..." << std::endl;

    // Cleanup resources
    if (device_) {
        // TODO: Properly destroy resources via RHI
        hzb_texture_ = rhi::handles::INVALID_RESOURCE;
        hzb_sampler_ = rhi::handles::INVALID_SAMPLER;
        hzb_copy_pipeline_ = rhi::handles::INVALID_PIPELINE;
        hzb_downsample_pipeline_ = rhi::handles::INVALID_PIPELINE;
        hzb_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }

    initialized_ = false;
}

bool HZBSystem::CreateHZBResources() {
    std::cout << "[HZBSystem] Creating HZB resources..." << std::endl;

    // Calculate number of mip levels
    u32 max_dim = std::max(config_.max_width, config_.max_height);
    mip_levels_ = 0;
    while (max_dim > config_.min_mip_size) {
        max_dim /= 2;
        mip_levels_++;
    }

    if (!CreateHZBTexture()) {
        return false;
    }

    std::cout << "[HZBSystem] HZB resources created successfully" << std::endl;
    return true;
}

bool HZBSystem::CreateHZBTexture() {
    rhi::TextureDesc hzbDesc{};
    hzbDesc.size = { config_.max_width, config_.max_height, 1 };
    hzbDesc.format = rhi::DataFormat::R32_Float;  // Color format for compute shader write access
    hzbDesc.type = rhi::TextureType::Texture2D;
    hzbDesc.mipLevels = mip_levels_;
    hzbDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;

    hzb_texture_ = device_->CreateTexture(hzbDesc);
    if (hzb_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[HZBSystem] Failed to create HZB texture" << std::endl;
        return false;
    }

    std::cout << "[HZBSystem] HZB texture created: " << config_.max_width << "x" << config_.max_height
              << " with " << mip_levels_ << " mip levels" << std::endl;

    return true;
}

bool HZBSystem::CreateHZBSampler() {
    rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = rhi::FilterMode::Linear;
    samplerDesc.magFilter = rhi::FilterMode::Linear;
    samplerDesc.mipFilter = rhi::FilterMode::Linear;
    samplerDesc.addressU = rhi::TextureAddressMode::Clamp;
    samplerDesc.addressV = rhi::TextureAddressMode::Clamp;
    samplerDesc.addressW = rhi::TextureAddressMode::Clamp;
    samplerDesc.maxLod = static_cast<float>(mip_levels_);

    hzb_sampler_ = device_->CreateSampler(samplerDesc);
    if (hzb_sampler_ == rhi::handles::INVALID_SAMPLER) {
        std::cerr << "[HZBSystem] Failed to create HZB sampler" << std::endl;
        return false;
    }

    return true;
}

bool HZBSystem::CreateHZBComputePipeline() {
    std::cout << "[HZBSystem] ========== Creating HZB Compute Pipeline ==========" << std::endl;

    rhi::DescriptorSetLayoutBinding hzbBindings[] = {
        { 0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute, nullptr },
        { 1, rhi::DescriptorType::StorageImage, 1, rhi::ShaderStage::Compute, nullptr }
    };

    rhi::DescriptorSetLayoutDesc layoutDesc{
        .bindings = hzbBindings,
        .bindingCount = 2
    };

    hzb_descriptor_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    if (hzb_descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[HZBSystem] ❌ Failed to create HZB descriptor layout" << std::endl;
        return false;
    }
    std::cout << "[HZBSystem] ✅ HZB descriptor layout created" << std::endl;

    // Create pipeline layout
    rhi::PipelineLayoutDesc pipelineLayoutDesc{
        .setLayoutCount = 1,
        .setLayouts = &hzb_descriptor_layout_,
        .pushConstantRangeCount = 0,
        .pushConstantRanges = nullptr
    };

    hzb_pipeline_layout_ = device_->CreatePipelineLayout(pipelineLayoutDesc);
    if (hzb_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[HZBSystem] Failed to create HZB pipeline layout" << std::endl;
        return false;
    }

    // Load HZB generation Metal shader
    std::string shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/HZBGeneration.metal";
    std::cout << "[HZBSystem] Loading shader from: " << shaderPath << std::endl;

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) {
        std::cerr << "[HZBSystem] ❌ Failed to open shader file: " << shaderPath << std::endl;
        return false;
    }
    std::cout << "[HZBSystem] ✅ Shader file opened successfully" << std::endl;

    std::string shaderCode((std::istreambuf_iterator<char>(shaderFile)),
                          std::istreambuf_iterator<char>());
    shaderFile.close();

    std::cout << "[HZBSystem] Shader code size: " << shaderCode.size() << " bytes" << std::endl;

    rhi::ShaderHandle copyShader = device_->CreateShader(
        shaderCode.data(),
        shaderCode.size(),
        rhi::ShaderStage::Compute,
        "copy_depth_to_hzb_mip0"
    );
    if (copyShader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[HZBSystem] ❌ Failed to create HZB base copy shader" << std::endl;
        return false;
    }

    rhi::ShaderHandle downsampleShader = device_->CreateShader(
        shaderCode.data(),
        shaderCode.size(),
        rhi::ShaderStage::Compute,
        "generate_hzb_mip_level_basic"
    );
    if (downsampleShader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[HZBSystem] ❌ Failed to create HZB downsample shader" << std::endl;
        return false;
    }

    rhi::ComputePipelineDesc pipelineDesc{};
    pipelineDesc.layout = hzb_pipeline_layout_;
    pipelineDesc.threadGroupSize = {16, 16, 1}; // Match HZB_THREAD_GROUP_SIZE in shader

    pipelineDesc.computeShader = copyShader;
    hzb_copy_pipeline_ = device_->CreateComputePipeline(pipelineDesc);
    if (hzb_copy_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[HZBSystem] ❌ Failed to create HZB copy pipeline" << std::endl;
        return false;
    }

    pipelineDesc.computeShader = downsampleShader;
    hzb_downsample_pipeline_ = device_->CreateComputePipeline(pipelineDesc);
    if (hzb_downsample_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[HZBSystem] ❌ Failed to create HZB downsample pipeline" << std::endl;
        return false;
    }

    std::cout << "[HZBSystem] ✅ HZB compute pipeline created successfully" << std::endl;
    std::cout << "[HZBSystem] ========== HZB Compute Pipeline Creation Complete ==========" << std::endl;
    return true;
}

HZBSystem::BuildResult HZBSystem::BuildHZB(rhi::ResourceHandle depth_texture,
                                          rhi::RHICommandBuffer* cmd_buffer) {
    if (!initialized_) {
        std::cerr << "[HZBSystem] Not initialized" << std::endl;
        return {};
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    BuildResult result;
    result.hzb_texture = hzb_texture_;
    result.mip_levels = mip_levels_;

    std::cout << "[HZBSystem] BuildHZB called: generate_on_gpu=" << config_.generate_on_gpu
              << ", cmd_buffer=" << (cmd_buffer ? "valid" : "null") << std::endl;

    if (config_.generate_on_gpu && cmd_buffer) {
        std::cout << "[HZBSystem] Attempting GPU HZB generation..." << std::endl;
        if (!GenerateHZBOnGPU(cmd_buffer, depth_texture)) {
            std::cerr << "[HZBSystem] ❌ GPU HZB generation failed, falling back to CPU" << std::endl;
            GenerateHZBOnCPU(depth_texture);
        }
    } else {
        std::cout << "[HZBSystem] Using CPU HZB generation (generate_on_gpu=" << config_.generate_on_gpu
                  << ", cmd_buffer=" << (cmd_buffer ? "valid" : "null") << ")" << std::endl;
        GenerateHZBOnCPU(depth_texture);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.build_time_ms = std::chrono::duration<float, std::milli>(end_time - start_time).count();

    last_result_ = result;

    std::cout << "[HZBSystem] HZB built in " << result.build_time_ms << " ms" << std::endl;

    return result;
}

bool HZBSystem::GenerateHZBOnCPU(rhi::ResourceHandle depth_texture) {
    std::cout << "[HZBSystem] CPU HZB generation - PLACEHOLDER" << std::endl;

    // TODO: Implement CPU-based HZB generation
    // 1. Read depth texture data
    // 2. Generate mip levels using max depth filtering
    // 3. Upload mip levels to GPU

    // For now, create a simple placeholder HZB
    return true;
}

bool HZBSystem::GenerateHZBOnGPU(rhi::RHICommandBuffer* cmd_buffer, rhi::ResourceHandle depth_texture) {
    std::cout << "[HZBSystem] ========== GPU HZB Generation Start ==========" << std::endl;

    if (hzb_copy_pipeline_ == rhi::handles::INVALID_PIPELINE ||
        hzb_downsample_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[HZBSystem] ❌ HZB compute pipelines not available" << std::endl;
        return false;
    }
    std::cout << "[HZBSystem] ✅ HZB compute pipelines are valid" << std::endl;

    if (depth_texture == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[HZBSystem] ❌ Invalid depth texture" << std::endl;
        return false;
    }
    std::cout << "[HZBSystem] ✅ Depth texture is valid: " << depth_texture << std::endl;

    if (hzb_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[HZBSystem] ❌ HZB texture is invalid" << std::endl;
        return false;
    }
    std::cout << "[HZBSystem] ✅ HZB texture is valid: " << hzb_texture_ << std::endl;

    std::vector<rhi::ResourceHandle> temporaryViews;

    rhi::DescriptorSetDesc descriptorDesc{};
    descriptorDesc.layout = hzb_descriptor_layout_;

    rhi::DescriptorSetHandle descriptorSet = device_->CreateDescriptorSet(descriptorDesc);
    if (descriptorSet == rhi::handles::INVALID_DESCRIPTOR_SET) {
        std::cerr << "[HZBSystem] Failed to create HZB descriptor set" << std::endl;
        return false;
    }

    rhi::TextureViewDesc baseTargetViewDesc{};
    baseTargetViewDesc.texture = hzb_texture_;
    baseTargetViewDesc.viewType = rhi::TextureType::Texture2D;
    baseTargetViewDesc.format = rhi::DataFormat::R32_Float;
    baseTargetViewDesc.mostDetailedMip = 0;
    baseTargetViewDesc.mipCount = 1;
    baseTargetViewDesc.firstArraySlice = 0;
    baseTargetViewDesc.arraySize = 1;
    rhi::ResourceHandle hzbMip0View = device_->CreateTextureView(baseTargetViewDesc);
    if (hzbMip0View == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[HZBSystem] Failed to create HZB mip0 view" << std::endl;
        device_->DestroyDescriptorSet(descriptorSet);
        return false;
    }
    temporaryViews.push_back(hzbMip0View);

    rhi::DescriptorImageInfo baseSourceInfo{};
    baseSourceInfo.imageView = depth_texture;
    baseSourceInfo.imageLayout = rhi::ResourceState::ShaderResource;
    baseSourceInfo.sampler = rhi::handles::INVALID_SAMPLER;
    rhi::DescriptorImageInfo baseTargetInfo{};
    baseTargetInfo.imageView = hzbMip0View;
    baseTargetInfo.imageLayout = rhi::ResourceState::UnorderedAccess;
    baseTargetInfo.sampler = rhi::handles::INVALID_SAMPLER;
    rhi::WriteDescriptorSet baseWrites[2]{};
    baseWrites[0].dstSet = descriptorSet;
    baseWrites[0].dstBinding = 0;
    baseWrites[0].descriptorCount = 1;
    baseWrites[0].descriptorType = rhi::DescriptorType::SampledImage;
    baseWrites[0].imageInfo = &baseSourceInfo;
    baseWrites[1].dstSet = descriptorSet;
    baseWrites[1].dstBinding = 1;
    baseWrites[1].descriptorCount = 1;
    baseWrites[1].descriptorType = rhi::DescriptorType::StorageImage;
    baseWrites[1].imageInfo = &baseTargetInfo;
    device_->UpdateDescriptorSets(2, baseWrites);

    const rhi::DescriptorSetHandle descriptorSets[] = { descriptorSet };
    cmd_buffer->BindComputePipeline(hzb_copy_pipeline_);
    cmd_buffer->BindDescriptorSets(
        rhi::PipelineBindPoint::Compute,
        hzb_pipeline_layout_,
        0, 1, descriptorSets,
        0, nullptr
    );
    // 🔇 DISABLED: Verbose HZB output
    // std::cout << "[HZBSystem] 🚀 Copying source depth to HZB mip 0" << std::endl;
    u32 threadGroupsX = (config_.max_width + 15) / 16;
    u32 threadGroupsY = (config_.max_height + 15) / 16;
    cmd_buffer->Dispatch(threadGroupsX, threadGroupsY, 1);
    cmd_buffer->MemoryBarrier(
        rhi::PipelineStage::ComputeShader,
        rhi::PipelineStage::ComputeShader,
        rhi::AccessFlag::ShaderWrite,
        rhi::AccessFlag::ShaderRead
    );

    for (u32 mip_level = 0; mip_level < mip_levels_ - 1; ++mip_level) {
        rhi::TextureViewDesc sourceViewDesc{};
        sourceViewDesc.texture = hzb_texture_;
        sourceViewDesc.viewType = rhi::TextureType::Texture2D;
        sourceViewDesc.format = rhi::DataFormat::R32_Float;
        sourceViewDesc.mostDetailedMip = mip_level;
        sourceViewDesc.mipCount = 1;
        sourceViewDesc.firstArraySlice = 0;
        sourceViewDesc.arraySize = 1;
        rhi::ResourceHandle sourceView = device_->CreateTextureView(sourceViewDesc);
        if (sourceView == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[HZBSystem] Failed to create source mip view" << std::endl;
            for (auto view : temporaryViews) device_->DestroyTexture(view);
            device_->DestroyDescriptorSet(descriptorSet);
            return false;
        }
        temporaryViews.push_back(sourceView);

        rhi::TextureViewDesc targetViewDesc{};
        targetViewDesc.texture = hzb_texture_;
        targetViewDesc.viewType = rhi::TextureType::Texture2D;
        targetViewDesc.format = rhi::DataFormat::R32_Float;
        targetViewDesc.mostDetailedMip = mip_level + 1;
        targetViewDesc.mipCount = 1;
        targetViewDesc.firstArraySlice = 0;
        targetViewDesc.arraySize = 1;
        rhi::ResourceHandle targetView = device_->CreateTextureView(targetViewDesc);
        if (targetView == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[HZBSystem] Failed to create target mip view" << std::endl;
            for (auto view : temporaryViews) device_->DestroyTexture(view);
            device_->DestroyDescriptorSet(descriptorSet);
            return false;
        }
        temporaryViews.push_back(targetView);

        rhi::DescriptorImageInfo sourceInfo{};
        sourceInfo.imageView = sourceView;
        sourceInfo.imageLayout = rhi::ResourceState::ShaderResource;
        sourceInfo.sampler = rhi::handles::INVALID_SAMPLER;
        rhi::DescriptorImageInfo targetInfo{};
        targetInfo.imageView = targetView;
        targetInfo.imageLayout = rhi::ResourceState::UnorderedAccess;
        targetInfo.sampler = rhi::handles::INVALID_SAMPLER;
        rhi::WriteDescriptorSet mipWrites[2]{};
        mipWrites[0].dstSet = descriptorSet;
        mipWrites[0].dstBinding = 0;
        mipWrites[0].descriptorCount = 1;
        mipWrites[0].descriptorType = rhi::DescriptorType::SampledImage;
        mipWrites[0].imageInfo = &sourceInfo;
        mipWrites[1].dstSet = descriptorSet;
        mipWrites[1].dstBinding = 1;
        mipWrites[1].descriptorCount = 1;
        mipWrites[1].descriptorType = rhi::DescriptorType::StorageImage;
        mipWrites[1].imageInfo = &targetInfo;
        device_->UpdateDescriptorSets(2, mipWrites);

        cmd_buffer->BindComputePipeline(hzb_downsample_pipeline_);
        cmd_buffer->BindDescriptorSets(
            rhi::PipelineBindPoint::Compute,
            hzb_pipeline_layout_,
            0, 1, descriptorSets,
            0, nullptr
        );

        u32 target_width = std::max(config_.max_width >> (mip_level + 1), 1u);
        u32 target_height = std::max(config_.max_height >> (mip_level + 1), 1u);
        threadGroupsX = (target_width + 15) / 16;
        threadGroupsY = (target_height + 15) / 16;

        // 🔇 DISABLED: Verbose HZB output
        // std::cout << "[HZBSystem] 🚀 Generating mip " << (mip_level + 1) << " from mip " << mip_level
        //           << " (" << target_width << "x" << target_height << ")" << std::endl;
        // std::cout << "[HZBSystem] Dispatch: " << threadGroupsX << "x" << threadGroupsY << "x1 thread groups" << std::endl;

        cmd_buffer->Dispatch(threadGroupsX, threadGroupsY, 1);
        cmd_buffer->MemoryBarrier(
            rhi::PipelineStage::ComputeShader,
            rhi::PipelineStage::ComputeShader,
            rhi::AccessFlag::ShaderWrite,
            rhi::AccessFlag::ShaderRead
        );

    }

    for (auto view : temporaryViews) device_->DestroyTexture(view);
    device_->DestroyDescriptorSet(descriptorSet);

    // 🔇 DISABLED: Verbose HZB output
    // std::cout << "[HZBSystem] ✅ HZB generation dispatch completed" << std::endl;
    // std::cout << "[HZBSystem] ========== GPU HZB Generation Complete ==========" << std::endl;

    return true;
}

bool HZBSystem::UpdateConfig(const Config& new_config) {
    std::cout << "[HZBSystem] Updating configuration..." << std::endl;

    // Check if dimensions changed
    if (new_config.max_width != config_.max_width ||
        new_config.max_height != config_.max_height ||
        new_config.min_mip_size != config_.min_mip_size) {

        // Need to recreate resources
        Shutdown();
        return Initialize(device_, new_config);
    }

    config_ = new_config;
    return true;
}

math::v2 HZBSystem::GetMipDimensions(u32 mip_level) const {
    if (mip_level >= mip_levels_) {
        return math::v2{static_cast<float>(config_.min_mip_size), static_cast<float>(config_.min_mip_size)};
    }

    u32 width = config_.max_width >> mip_level;
    u32 height = config_.max_height >> mip_level;

    // Clamp to minimum size
    width = std::max(width, config_.min_mip_size);
    height = std::max(height, config_.min_mip_size);

    return math::v2{static_cast<float>(width), static_cast<float>(height)};
}

// HZB Occlusion Culling Implementation

bool HZBOcclusionCulling::TestBoundingBox(const math::v3& bbox_min, const math::v3& bbox_max,
                                         const math::m4x4& view_projection,
                                         const HZBSystem& hzb_system) {
    if (!hzb_system.IsReady()) {
        return true; // Conservative: assume visible if HZB not ready
    }

    // Calculate bbox center
    math::v3 bbox_center = (bbox_min + bbox_max) * 0.5f;

    // Transform center to clip space
    math::v4 temp_vec = {bbox_center.x, bbox_center.y, bbox_center.z, 1.0f};
    math::v4 clip_center = view_projection * temp_vec;

    if (clip_center.w <= 0.0f) {
        return true; // Behind camera
    }

    // TODO: Implement proper HZB depth testing
    // For now, be conservative and return true
    return true;
}

bool HZBOcclusionCulling::TestSphere(const math::v3& sphere_center, float sphere_radius,
                                    const math::m4x4& view_projection,
                                    const HZBSystem& hzb_system) {
    if (!hzb_system.IsReady()) {
        return true; // Conservative: assume visible if HZB not ready
    }

    // Convert sphere to bounding box for testing
    math::v3 radius_vec = {sphere_radius, sphere_radius, sphere_radius};
    math::v3 bbox_min = sphere_center - radius_vec;
    math::v3 bbox_max = sphere_center + radius_vec;

    return TestBoundingBox(bbox_min, bbox_max, view_projection, hzb_system);
}

HZBOcclusionResult HZBOcclusionCulling::TestBatch(const std::vector<std::pair<math::v3, math::v3>>& bounding_boxes,
                                                 const math::m4x4& view_projection,
                                                 const HZBSystem& hzb_system,
                                                 std::vector<bool>& out_visibility) {
    HZBOcclusionResult result{};
    result.total_objects_tested = static_cast<u32>(bounding_boxes.size());

    auto start_time = std::chrono::high_resolution_clock::now();

    out_visibility.resize(bounding_boxes.size());

    for (size_t i = 0; i < bounding_boxes.size(); ++i) {
        bool visible = TestBoundingBox(bounding_boxes[i].first, bounding_boxes[i].second, view_projection, hzb_system);
        out_visibility[i] = visible;

        if (visible) {
            result.objects_visible++;
        } else {
            result.objects_occluded++;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.culling_time_ms = std::chrono::duration<float, std::milli>(end_time - start_time).count();

    std::cout << "[HZBOcclusion] Tested " << result.total_objects_tested
              << " objects: " << result.objects_visible << " visible, "
              << result.objects_occluded << " occluded ("
              << result.culling_time_ms << " ms)" << std::endl;

    return result;
}

} // namespace primal::graphics::nanite
