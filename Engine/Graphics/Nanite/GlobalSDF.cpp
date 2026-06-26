#include "GlobalSDF.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include "../RHI/Platforms/Metal/MetalDevice.h"
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
        // Even when Initialize() never ran, a prior SetDataProvider() may have
        // plugged in a provider. Reset it here while device_ is still non-null
        // (set by Initialize on first init) — the provider's destructor calls
        // device_->Destroy* and needs a live device. Singleton destruction at
        // program-exit runs after the test has freed its RHI device, so leaving
        // the provider alive guarantees use-after-free on shutdown.
        data_provider_.reset();
        return;
    }

    UnregisterFromFieldRegistry();

    // Tear down data_provider_ BEFORE freeing cascades/device — its destructor
    // issues device_->Destroy* calls that require a live device.
    data_provider_.reset();

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

    cascade.extent = math::v3{
        cascade.resolution * cascade.voxel_size,
        cascade.resolution * cascade.voxel_size,
        cascade.resolution * cascade.voxel_size
    };

    // When a data_provider_ is set (Strategy path), freeze cascade origin
    // centered on world origin so the provider's SDF data stays stable under
    // camera movement. Without this lock, CalculateCascadeOrigin snaps the
    // cascade to a grid based on camera position; when the camera crosses a
    // cascade_size boundary the origin jumps, shifting SDF data out from
    // under the fixed-bounds SurfaceNets mesh. Result: mesh topology changes
    // every snap, geometry flickers / goes solid at mesh bounds.
    // Freezing here means AnalyticSDFProvider (sphere at origin) writes the
    // same texel values each frame, SurfaceNets samples the same world
    // positions, mesh stays stable regardless of camera movement.
    if (data_provider_) {
        cascade.origin = math::v3{
            -cascade.extent.x * 0.5f,
            -cascade.extent.y * 0.5f,
            -cascade.extent.z * 0.5f
        };
        return;
    }

    cascade.origin = CalculateCascadeOrigin(cascade.cascade_index, camera_position, cascade.voxel_size);
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

bool GlobalSDF::IsVoxelizationReady() const {
    const bool provider_ready = (data_provider_ && data_provider_->IsReady());
    static int s_checks = 0;
    if (s_checks < 3) {
        std::cout << "[GlobalSDF] IsVoxelizationReady check #" << s_checks
                  << " vox_pipe=" << voxelization_ready_
                  << " provider=" << (data_provider_ ? "set" : "null")
                  << " provider_ready=" << (provider_ready ? "yes" : "no")
                  << " -> " << (voxelization_ready_ || provider_ready ? "TRUE" : "FALSE")
                  << std::endl;
        ++s_checks;
    }
    return voxelization_ready_ || provider_ready;
}

void GlobalSDF::DispatchVoxelization(rhi::RHICommandBuffer* cmd, u32 cascade_index) {
    if (!cmd) return;
    if (cascade_index >= cascades_.size()) return;

    static int s_dv_calls = 0;
    if (s_dv_calls < 3) {
        std::cout << "[GlobalSDF] DispatchVoxelization #" << s_dv_calls
                  << " cascade=" << cascade_index
                  << " provider_set=" << (data_provider_ ? "yes" : "no")
                  << " provider_ready=" << (data_provider_ && data_provider_->IsReady() ? "yes" : "no")
                  << " vox_ready=" << voxelization_ready_
                  << std::endl;
        ++s_dv_calls;
    }

    const auto& cascade = cascades_[cascade_index];
    if (!cascade.is_valid || cascade.sdf_texture == rhi::handles::INVALID_RESOURCE) return;

    // Strategy path — preferred when a provider is plugged in (e.g., editor_mode
    // uses AnalyticSDFProvider; runtime Nanite path leaves this null and uses
    // the legacy vox_pipeline_ below).
    if (data_provider_ && data_provider_->IsReady()) {
        // Use per-frame counter for triple-buffer slot, NOT cascade_index%3.
        // With only cascade 0 dispatched, cascade_index%3 is always 0, which
        // causes params_cb_[0] to be rewritten every frame while the previous
        // frame's GPU dispatch is still reading it — CPU-GPU race that
        // corrupts SDF data and destabilizes the SurfaceNets mesh.
        const u32 frame_slot = provider_frame_counter_++ % 3;
        data_provider_->DispatchCascade(cmd, frame_slot, cascade);
        return;
    }

    if (!voxelization_ready_) return;

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

// --- f32 → f16 bit conversion (IEEE 754 half) ---
// Used by DebugFill to write R16_Float cascade textures from CPU.
static inline u16 f32_to_f16(f32 f) {
    u32 x;
    std::memcpy(&x, &f, sizeof(x));
    const u32 sign = (x >> 16) & 0x8000;
    const int e = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const u32 m = x & 0x7FFFFF;

    if (e <= 0) {
        // Denormal / underflow → zero (good enough for debug SDF)
        return static_cast<u16>(sign);
    }
    if (e >= 31) {
        // Inf / NaN / overflow → Inf
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (e << 10) | (m >> 13));
}

bool GlobalSDF::DebugFill(std::function<f32(const math::v3&)> sdf_fn) {
    if (!initialized_ || !device_) return false;
    if (!sdf_fn) return false;

    // Find max cascade resolution to size the staging buffer once.
    u32 max_res = 0;
    for (const auto& c : cascades_) {
        if (c.resolution > max_res) max_res = c.resolution;
    }
    if (max_res == 0) return false;

    const u64 staging_bytes = static_cast<u64>(max_res) * max_res * max_res * sizeof(u16);
    rhi::BufferDesc bufDesc{
        staging_bytes,
        rhi::BufferType::Unknown,
        rhi::GPUMemoryUsage::Staging,
        rhi::GPUMemoryUsage::Staging,
        0,
    };
    rhi::ResourceHandle staging = device_->CreateBuffer(bufDesc);
    if (staging == rhi::handles::INVALID_RESOURCE) return false;

    // Scratch CPU buffer for the largest cascade; reused for smaller ones.
    utl::vector<u16> cpu_data;
    cpu_data.resize(max_res * max_res * max_res);

    bool all_ok = true;

    for (const auto& c : cascades_) {
        if (c.sdf_texture == rhi::handles::INVALID_RESOURCE) continue;
        if (!c.is_valid) continue;

        const u32 res = c.resolution;
        const f32 inv_res = 1.0f / static_cast<f32>(res);
        // Voxel center world position = origin + (voxel + 0.5) * extent / res
        const math::v3 voxel_step{c.extent.x * inv_res,
                                   c.extent.y * inv_res,
                                   c.extent.z * inv_res};

        for (u32 z = 0; z < res; ++z) {
            for (u32 y = 0; y < res; ++y) {
                for (u32 x = 0; x < res; ++x) {
                    const math::v3 p{
                        c.origin.x + (static_cast<f32>(x) + 0.5f) * voxel_step.x,
                        c.origin.y + (static_cast<f32>(y) + 0.5f) * voxel_step.y,
                        c.origin.z + (static_cast<f32>(z) + 0.5f) * voxel_step.z};
                    const f32 d = sdf_fn(p);
                    cpu_data[(z * res + y) * res + x] = f32_to_f16(d);
                }
            }
        }

        const u64 cascade_bytes = static_cast<u64>(res) * res * res * sizeof(u16);
        if (!device_->UpdateBufferData(staging, cpu_data.data(), cascade_bytes, 0)) {
            all_ok = false;
            continue;
        }

        auto cmdHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        // Bypass rhi::GetCommandBuffer (global singleton): test binaries that use
        // C++ engine APIs link both libEngine.a (static) and libEngineDLL.dylib,
        // producing two singleton instances. The dylib registers, the static
        // reads — lookup fails. MetalDevice::GetCommandBuffer routes through
        // the device's own allocator and is singleton-free.
        auto* metal_dev = dynamic_cast<rhi::MetalDevice*>(device_);
        auto* cmd = metal_dev ? metal_dev->GetCommandBuffer(cmdHandle) : nullptr;
        if (!cmd) { all_ok = false; continue; }

        cmd->Begin();
        rhi::BufferTextureCopyRegion region;
        region.bufferOffset = 0;
        region.bufferRowLength = 0;  // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {res, res, res};
        cmd->CopyBufferToTexture(staging, c.sdf_texture, &region, 1);
        cmd->End();

        rhi::QueueSubmitInfo submit{};
        submit.cmdBuffer = cmdHandle;
        device_->Submit(submit);
        cmd->WaitForCompletion();
    }

    device_->DestroyBuffer(staging);
    return all_ok;
}

} // namespace primal::graphics::nanite
