#include "GlobalSDF.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include "Graphics/Field/FieldRegistry.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <set>
#include <iostream>

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
    RegisterToFieldRegistry();
    return true;
}

void GlobalSDF::Shutdown() {
    if (!initialized_) {
        return;
    }

    UnregisterFromFieldRegistry();

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

    // Update field registry with latest cascade data (origins may have shifted)
    auto descs = GetCascadeDescriptors();
    auto& registry = field::FieldRegistry::Get();
    for (u32 i = 0; i < config_.cascade_count && i < 4; ++i) {
        registry.Update(field::FieldSemantic::GlobalSDF, descs[i]);
    }
}

bool GlobalSDF::CreateCascades() {
    for (auto& cascade : cascades_) {
        u32 resolution = cascade.resolution;
        // SDF cascade only needs mip 0 — voxelization writes only to mip 0,
        // and DDGI trace reads only mip 0. Extra mip levels waste memory
        // and contain uninitialized garbage data.
        u32 mip_levels = 1;
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
    desc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
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

    // Snap camera to grid, then offset by -half to CENTER the cascade on the camera.
    // This ensures geometry at the camera position is covered even if it extends
    // in the negative direction (e.g. Sponza centered at origin).
    f32 half = cascade_size * 0.5f;

    math::v3 snapped_pos{
        std::floor((camera_position.x + half) / cascade_size) * cascade_size - half,
        std::floor((camera_position.y + half) / cascade_size) * cascade_size - half,
        std::floor((camera_position.z + half) / cascade_size) * cascade_size - half
    };

    return snapped_pos;
}

f32 GlobalSDF::CalculateCascadeVoxelSize(u32 cascade_index) const {
    return config_.voxel_size_base * std::pow(config_.cascade_scale_factor, cascade_index);
}

// ============================================================================
// Field System integration
// ============================================================================

GlobalSDF::CascadeDescriptors GlobalSDF::GetCascadeDescriptors() const {
    CascadeDescriptors descs{};
    for (u32 i = 0; i < config_.cascade_count && i < 4; ++i) {
        const auto& c = cascades_[i];
        auto& d = descs[i];

        d.type = field::FieldType::SDF;
        d.semantic = field::FieldSemantic::GlobalSDF;
        d.origin = c.origin;
        d.extent = c.extent;
        d.is_valid = c.is_valid;

        d.Set(field::FieldAttr::VoxelSize, c.voxel_size);
        d.Set(field::FieldAttr::Resolution, c.resolution);
        d.Set(field::FieldAttr::MipLevels, c.mip_levels);
        d.Set(field::FieldAttr::CascadeIndex, i);
        d.Set(field::FieldAttr::CascadeCount, config_.cascade_count);
        d.Set(field::FieldAttr::CascadeScale, static_cast<f32>(config_.cascade_scale_factor));

        d.SetResource(field::FieldResourceSlot::Primary, c.sdf_texture);
    }
    return descs;
}

void GlobalSDF::RegisterToFieldRegistry() {
    auto& registry = field::FieldRegistry::Get();
    auto descs = GetCascadeDescriptors();
    for (u32 i = 0; i < config_.cascade_count && i < 4; ++i) {
        registry.Register(descs[i]);
    }
}

void GlobalSDF::UnregisterFromFieldRegistry() {
    field::FieldRegistry::Get().Unregister(field::FieldSemantic::GlobalSDF);
}

const SDFCascade& GlobalSDF::GetCascade(u32 index) const {
    static SDFCascade invalid_cascade{};
    if (index >= cascades_.size()) {
        return invalid_cascade;
    }
    return cascades_[index];
}

// ============================================================================
// Shader loading (same pattern as LumenDDGIPass)
// ============================================================================

namespace {

static const std::string SDF_SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Nanite/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::vector<u8> LoadShaderSource(const char* name) {
    std::string path = SDF_SHADER_DIR + std::string(name) + ".metal";
    std::string source = ReadFileToString(path);
    if (source.empty()) {
        std::cerr << "[GlobalSDF] Failed to load shader: " << path << std::endl;
        return {};
    }
    return std::vector<u8>(source.begin(), source.end());
}

struct DescriptorData {
    u32 binding;
    rhi::DescriptorType type;
    rhi::ResourceHandle resource;
    u32 count{ 1 };
};

static void UpdateDescriptorSet(rhi::RHIDeviceBase* device, rhi::DescriptorSetHandle set,
                                 const DescriptorData* params, u32 count) {
    utl::vector<rhi::WriteDescriptorSet> writes(count);
    utl::vector<rhi::DescriptorBufferInfo> bufferInfos(count);
    utl::vector<rhi::DescriptorImageInfo> imageInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == rhi::DescriptorType::UniformBuffer ||
            params[i].type == rhi::DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == rhi::DescriptorType::StorageImage ||
                   params[i].type == rhi::DescriptorType::SampledImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = rhi::ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

} // anonymous namespace

// ============================================================================
// InitVoxelization
// ============================================================================

bool GlobalSDF::InitVoxelization(const SDFVoxelizationResources& resources) {
    if (!initialized_ || !device_) return false;

    vox_resources_ = resources;

    // Load shader source
    auto code = LoadShaderSource("GlobalSDFVoxelization");
    if (code.empty()) {
        std::cerr << "[GlobalSDF] Failed to load voxelization shader" << std::endl;
        return false;
    }

    auto shader = device_->CreateShader(code.data(), code.size(),
                                         rhi::ShaderStage::Compute, "voxelize_sdf");
    if (shader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[GlobalSDF] Failed to compile voxelization shader" << std::endl;
        return false;
    }

    // Create descriptor set layout
    // Metal: texture(0) = SDF output, buffer(0..6) = cascade + geometry data
    {
        rhi::DescriptorSetLayoutBinding bindings[] = {
            // Texture
            {0, rhi::DescriptorType::StorageImage,  1, rhi::ShaderStage::Compute, nullptr},
            // Buffers (separate Metal namespace)
            {0, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // CascadeUniforms
            {1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // vertex positions
            {2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // meshlets
            {3, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // meshlet vertex indices
            {4, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // meshlet triangle indices
            {5, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // cluster map
            {6, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr},  // instance data
        };
        rhi::DescriptorSetLayoutDesc layoutDesc{8, bindings};
        vox_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // Create pipeline layout
    {
        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &vox_set_layout_;
        vox_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipeline
    {
        rhi::ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader;
        pipeDesc.layout = vox_layout_;
        pipeDesc.threadGroupSize = {4, 4, 4};
        vox_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    if (vox_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[GlobalSDF] Failed to create voxelization pipeline" << std::endl;
        return false;
    }

    // Create triple-buffered cascade constant buffers
    for (int i = 0; i < 3; i++) {
        // Cascade uniforms (matches Metal CascadeUniforms struct)
        rhi::BufferDesc cbDesc{};
        cbDesc.size = 64;  // CascadeUniforms: float4 + uint3 + uint + padding = 48 bytes, round to 64
        cbDesc.type = rhi::BufferType::Constant;
        cbDesc.usage = rhi::GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        vox_cascade_cb_[i] = device_->CreateBuffer(cbDesc);

        // Descriptor sets
        rhi::DescriptorSetDesc dsDesc{vox_set_layout_};
        vox_descriptor_sets_[i] = device_->CreateDescriptorSet(dsDesc);
    }

    voxelization_ready_ = true;
    std::cout << "[GlobalSDF] Voxelization pipeline ready (" << resources.num_instances
              << " instances)" << std::endl;
    return true;
}

// ============================================================================
// DispatchVoxelization
// ============================================================================

void GlobalSDF::DispatchVoxelization(rhi::RHICommandBuffer* cmd, u32 cascade_index) {
    if (!voxelization_ready_ || !cmd) return;
    if (cascade_index >= cascades_.size()) return;

    const auto& cascade = cascades_[cascade_index];
    if (!cascade.is_valid || cascade.sdf_texture == rhi::handles::INVALID_RESOURCE) return;

    // Determine frame index for triple-buffered resources
    u32 frameIdx = cascade_index % 3;

    // Upload cascade uniforms
    {
        // Must match Metal CascadeUniforms layout EXACTLY:
        // offset 0:  float4 origin (xyz=origin, w=voxel_size) — 16 bytes
        // offset 16: uint res_x, res_y, res_z              — 12 bytes
        // offset 28: uint num_instances                  — 4 bytes
        // Total: 32 bytes
        struct alignas(16) CascadeUniformsCB {
            f32 origin_x, origin_y, origin_z, voxel_size;  // offset 0-15
            u32 res_x, res_y, res_z;                      // offset 16-27
            u32 num_instances;                            // offset 28-31
            u32 _pad[4];                                     // pad to 48 (align to 16)
        };

        auto* mapped = static_cast<CascadeUniformsCB*>(device_->MapBuffer(vox_cascade_cb_[frameIdx]));
        if (mapped) {
            mapped->origin_x = cascade.origin.x;
            mapped->origin_y = cascade.origin.y;
            mapped->origin_z = cascade.origin.z;
            mapped->voxel_size = cascade.voxel_size;
            mapped->res_x = cascade.resolution;
            mapped->res_y = cascade.resolution;
            mapped->res_z = cascade.resolution;
            mapped->num_instances = vox_resources_.num_instances;
            device_->UnmapBuffer(vox_cascade_cb_[frameIdx]);
        } else {
            static bool logMapFail = false;
            if (!logMapFail) {
                std::cerr << "[GlobalSDF] ERROR: MapBuffer failed for cascade CB!" << std::endl;
                logMapFail = true;
            }
        }
    }

    // Update descriptor set
    {
        DescriptorData params[] = {
            // Texture (SDF output)
            {0, rhi::DescriptorType::StorageImage, cascade.sdf_texture},
            // Buffers
            {0, rhi::DescriptorType::UniformBuffer, vox_cascade_cb_[frameIdx]},
            {1, rhi::DescriptorType::StorageBuffer, vox_resources_.vertex_buffer},
            {2, rhi::DescriptorType::StorageBuffer, vox_resources_.meshlet_buffer},
            {3, rhi::DescriptorType::StorageBuffer, vox_resources_.meshlet_vertices_buffer},
            {4, rhi::DescriptorType::StorageBuffer, vox_resources_.meshlet_triangles_buffer},
            {5, rhi::DescriptorType::StorageBuffer, vox_resources_.cluster_map_buffer},
            {6, rhi::DescriptorType::StorageBuffer, vox_resources_.instance_data_buffer},
        };
        UpdateDescriptorSet(device_, vox_descriptor_sets_[frameIdx], params, 8);
    }

    // Bind and dispatch
    cmd->BindComputePipeline(vox_pipeline_);
    const rhi::DescriptorSetHandle sets[] = { vox_descriptor_sets_[frameIdx] };
    cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, vox_layout_, 0, 1, sets, 0, nullptr);

    // Dispatch: (resolution/4, resolution/4, resolution/4) groups with threadGroupSize (4,4,4)
    u32 res = cascade.resolution;
    u32 gx = (res + 3) / 4;
    u32 gy = (res + 3) / 4;
    u32 gz = (res + 3) / 4;
    cmd->Dispatch(gx, gy, gz);

    // Barrier: SDF texture UAV → SRV (for DDGI trace to read)
    {
        rhi::ResourceBarrier barrier{};
        barrier.resource = cascade.sdf_texture;
        barrier.beforeState = rhi::ResourceState::UnorderedAccess;
        barrier.afterState = rhi::ResourceState::ShaderResource;
        barrier.subresource = 0xFFFFFFFF;
        cmd->InsertBarrier(&barrier, 1);
    }
}

} // namespace primal::graphics::nanite
