#include "GlobalSDF.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHIMath.h"
#include <algorithm>
#include <chrono>

namespace primal::graphics::nanite {

GlobalSDF& GlobalSDF::Get() {
    static GlobalSDF instance;
    return instance;
}

bool GlobalSDF::Initialize(rhi::RHIDeviceBase* device, const GlobalSDFConfig& config) {
    if (initialized_) {
        return true;
    }
    
    if (!device) {
        return false;
    }
    
    device_ = device;
    config_ = config;
    
    cascades_.resize(config.cascade_count);
    
    for (u32 i = 0; i < config.cascade_count; ++i) {
        cascades_[i].cascade_index = i;
        cascades_[i].resolution = config.base_resolution;
        cascades_[i].voxel_size = config.voxel_size_base * 
                          std::pow(config.cascade_scale_factor, i);
    }
    
    if (!CreateCascades()) {
        return false;
    }
    
    if (!CreateGlobalTexture()) {
        return false;
    }
    
    initialized_ = true;
    return true;
}

void GlobalSDF::Shutdown() {
    if (!initialized_) {
        return;
    }
    
    for (auto& cascade : cascades_) {
        if (cascade.sdf_texture != rhi::handles::INVALID_RESOURCE) {
            FreeTexture(cascade.sdf_texture);
            cascade.sdf_texture = rhi::handles::INVALID_RESOURCE;
        }
    }
    
    if (global_sdf_texture_ != rhi::handles::INVALID_RESOURCE) {
        FreeTexture(global_sdf_texture_);
        global_sdf_texture_ = rhi::handles::INVALID_RESOURCE;
    }
    
    cascades_.clear();
    device_ = nullptr;
    initialized_ = false;
}

void GlobalSDF::Update(const RenderSceneSnapshot& snapshot, u64 current_frame, 
                       const math::v3& camera_position) {
    if (!initialized_ || !device_) {
        return;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    u32 updates_this_frame = 0;
    f32 budget_remaining_ms = config_.update_budget_ms;
    
    for (u32 i = 0; i < cascades_.size(); ++i) {
        auto& cascade = cascades_[i];
        
        f32 distance = rhi::math::Length(camera_position - cascade.origin);
        cascade.needs_update = (distance < config_.max_cascade_distance);
        
        if (cascade.needs_update && budget_remaining_ms > 0.0f) {
            auto cascade_start = std::chrono::high_resolution_clock::now();
            
            UpdateCascade(cascade, snapshot, camera_position);
            cascade.last_update_frame = current_frame;
            
            auto cascade_end = std::chrono::high_resolution_clock::now();
            auto cascade_duration = std::chrono::duration_cast<std::chrono::microseconds>(
                cascade_end - cascade_start);
            f32 cascade_time_ms = static_cast<f32>(cascade_duration.count()) / 1000.0f;
            
            budget_remaining_ms -= cascade_time_ms;
            updates_this_frame++;
        } else if (cascade.needs_update) {
            stats_.skipped_updates++;
        }
    }
    
    MergeCascades();
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end_time - start_time);
    
    stats_.update_time_ms = static_cast<f32>(total_duration.count()) / 1000.0f;
    stats_.updates_this_frame = updates_this_frame;
    
    UpdateStats(current_frame);
}

bool GlobalSDF::CreateCascades() {
    for (auto& cascade : cascades_) {
        u32 resolution = cascade.resolution;
        u32 mip_levels = static_cast<u32>(std::log2(resolution)) + 1;
        cascade.mip_levels = mip_levels;
        
        if (!AllocateTexture(cascade.sdf_texture, resolution, mip_levels)) {
            return false;
        }
        
        cascade.is_valid = true;
    }
    
    return true;
}

bool GlobalSDF::CreateGlobalTexture() {
    u32 max_resolution = 0;
    for (const auto& cascade : cascades_) {
        if (cascade.resolution > max_resolution) {
            max_resolution = cascade.resolution;
        }
    }
    
    u32 mip_levels = static_cast<u32>(std::log2(max_resolution)) + 1;
    
    return AllocateTexture(global_sdf_texture_, max_resolution, mip_levels);
}

void GlobalSDF::UpdateCascade(SDFCascade& cascade, const RenderSceneSnapshot& snapshot,
                              const math::v3& camera_position) {
    if (!cascade.is_valid) {
        return;
    }
    
    cascade.origin = CalculateCascadeOrigin(cascade.cascade_index, camera_position, cascade.voxel_size);
    
    cascade.extent = math::v3{
        cascade.resolution * cascade.voxel_size,
        cascade.resolution * cascade.voxel_size,
        cascade.resolution * cascade.voxel_size
    };
}

void GlobalSDF::MergeCascades() {
}

void GlobalSDF::UpdateStats(u64 current_frame) {
    stats_.cascade_count = static_cast<u32>(cascades_.size());
    stats_.active_cascades = 1;
    
    for (const auto& cascade : cascades_) {
        if (cascade.is_valid) {
            stats_.active_cascades++;
        }
    }
    
    u32 total_bytes = 1;
    for (const auto& cascade : cascades_) {
        if (cascade.sdf_texture != rhi::handles::INVALID_RESOURCE) {
            total_bytes += cascade.resolution * cascade.resolution * cascade.resolution;
        }
    }
    
    if (global_sdf_texture_ != rhi::handles::INVALID_RESOURCE) {
        u32 global_res = config_.base_resolution;
        total_bytes += global_res * global_res * global_res;
    }
    
    stats_.total_memory_mb = total_bytes / (1024 * 1024);
}

bool GlobalSDF::AllocateTexture(rhi::ResourceHandle& handle, u32 resolution, u32 mip_levels) {
    rhi::TextureDesc desc{};
    desc.size = {resolution, resolution, resolution};
    desc.mipLevels = mip_levels;
    desc.arraySize = 1;
    desc.format = rhi::DataFormat::R16_Float;
    desc.type = rhi::TextureType::Texture3D;
    desc.usage = rhi::TextureUsage::ShaderResource;
    desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    
    handle = device_->CreateTexture(desc);
    return handle != rhi::handles::INVALID_RESOURCE;
}

void GlobalSDF::FreeTexture(rhi::ResourceHandle& handle) {
    if (device_ && handle != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(handle);
        handle = rhi::handles::INVALID_RESOURCE;
    }
}

u32 GlobalSDF::CalculateRequiredResolution(f32 distance, u32 cascade_index) const {
    if (!config_.enable_adaptive_quality) {
        return config_.base_resolution;
    }
    
    f32 distance_factor = std::max(1.0f, distance / 100.0f);
    u32 resolution = static_cast<u32>(config_.base_resolution / distance_factor);
    
    resolution = std::max(32u, std::min(resolution, config_.base_resolution));
    
    return resolution;
}

math::v3 GlobalSDF::CalculateCascadeOrigin(u32 cascade_index, const math::v3& camera_position, 
                                            f32 voxel_size) const {
    f32 cascade_size = config_.base_resolution * voxel_size;
    f32 half_size = cascade_size / 2.0f;
    
    math::v3 snapped_pos{
        std::floor(camera_position.x / cascade_size) * cascade_size + half_size,
        std::floor(camera_position.y / cascade_size) * cascade_size + half_size,
        std::floor(camera_position.z / cascade_size) * cascade_size + half_size
    };
    
    return snapped_pos;
}

f32 GlobalSDF::CalculateCascadeVoxelSize(u32 cascade_index) const {
    return config_.voxel_size_base * std::pow(config_.cascade_scale_factor, cascade_index);
}

const SDFCascade& GlobalSDF::GetCascade(u32 index) const {
    static SDFCascade invalid_cascade{};
    if (index >= cascades_.size()) {
        return invalid_cascade;
    }
    return cascades_[index];
}

} // namespace primal::graphics::nanite
