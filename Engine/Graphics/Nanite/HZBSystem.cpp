#include "HZBSystem.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include <algorithm>
#include <cmath>
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
        hzb_compute_pipeline_ = rhi::handles::INVALID_PIPELINE;
        hzb_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }

    // Cleanup frame resources
    for (auto& frame_res : frame_resources_) {
        frame_res.staging_buffer = rhi::handles::INVALID_RESOURCE;
        frame_res.in_use = false;
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

    // Create staging buffers for each frame resource
    for (auto& frame_res : frame_resources_) {
        rhi::BufferDesc stagingDesc{};
        stagingDesc.size = config_.max_width * config_.max_height * sizeof(f32); // Single channel depth
        stagingDesc.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
        stagingDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;

        frame_res.staging_buffer = device_->CreateBuffer(stagingDesc);
        if (frame_res.staging_buffer == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[HZBSystem] Failed to create staging buffer" << std::endl;
            return false;
        }

        frame_res.in_use = false;
    }

    std::cout << "[HZBSystem] HZB resources created successfully" << std::endl;
    return true;
}

bool HZBSystem::CreateHZBTexture() {
    rhi::TextureDesc hzbDesc{};
    hzbDesc.size = { config_.max_width, config_.max_height, 1 };
    hzbDesc.format = rhi::DataFormat::R32_Float;  // Single channel depth
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
    // TODO: Implement compute shader for HZB generation
    // For now, return false to force CPU fallback
    std::cout << "[HZBSystem] GPU HZB generation not implemented, using CPU fallback" << std::endl;
    return false;
}

HZBSystem::BuildResult HZBSystem::BuildHZB(rhi::ResourceHandle depth_texture,
                                          rhi::RHICommandBuffer* cmd_buffer,
                                          u32 frame_index) {
    if (!initialized_) {
        std::cerr << "[HZBSystem] Not initialized" << std::endl;
        return {};
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    BuildResult result;
    result.hzb_texture = hzb_texture_;
    result.mip_levels = mip_levels_;

    // Get current frame resource
    current_frame_resource_ = frame_index % frame_resources_.size();

    if (config_.generate_on_gpu && cmd_buffer) {
        if (!GenerateHZBOnGPU(cmd_buffer, depth_texture)) {
            std::cerr << "[HZBSystem] GPU HZB generation failed, falling back to CPU" << std::endl;
            GenerateHZBOnCPU(depth_texture);
        }
    } else {
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
    std::cout << "[HZBSystem] GPU HZB generation - PLACEHOLDER" << std::endl;

    // TODO: Implement GPU-based HZB generation using compute shaders
    // 1. Copy depth texture to HZB base mip level
    // 2. For each subsequent mip level:
    //    - Dispatch compute shader to sample previous mip and write max depth
    //    - Insert appropriate memory barriers

    return false; // Not implemented yet
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