#include "Graphics/PCG/GPU/GPUMesher.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

namespace primal::graphics::pcg {

namespace {

// Uniform struct — must match SurfaceNetsUniforms in SurfaceNetsGPU.metal exactly.
struct SurfaceNetsUniforms {
    u32  resolution;
    u32  n;
    u32  n2;
    f32  voxel_x, voxel_y, voxel_z;
    f32  origin_x, origin_y, origin_z;
    f32  extent_x, extent_y, extent_z;
    f32  iso_value;
    u32  pad0, pad1, pad2;
};

struct ScratchBuffers {
    rhi::ResourceHandle uniforms{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle scalar_volume{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dual_id{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle positions{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle elements{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle indices{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle counters{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle indirect_args{rhi::handles::INVALID_RESOURCE};
};

void DestroyScratch(rhi::RHIDeviceBase* dev, ScratchBuffers& s) {
    if (s.uniforms       != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.uniforms);
    if (s.scalar_volume  != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.scalar_volume);
    if (s.dual_id        != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.dual_id);
    if (s.positions      != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.positions);
    if (s.elements       != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.elements);
    if (s.indices        != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.indices);
    if (s.counters       != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.counters);
    if (s.indirect_args  != rhi::handles::INVALID_RESOURCE) dev->DestroyBuffer(s.indirect_args);
    s = ScratchBuffers{};
}

// Read self-contained .metal source as raw bytes. Pattern: LumenSSAOPass::LoadShaderBytecode
// minus the #include resolver (our shader has no engine includes).
std::vector<u8> LoadShaderSource(const char* shader_name) {
    const std::string rel_path = std::string("Engine/Graphics/Metal/shaders/PCG/") + shader_name + ".metal";

    // Try 1: relative to CWD (works when test/bin runs from project root).
    {
        std::ifstream f(rel_path, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string s = ss.str();
            return std::vector<u8>(s.begin(), s.end());
        }
    }

    // Try 2: walk up CWD ancestors looking for an "Engine/" directory.
    namespace fs = std::filesystem;
    fs::path p = fs::current_path();
    for (int i = 0; i < 8 && p.has_parent_path(); ++i) {
        fs::path candidate = p / rel_path;
        if (fs::exists(candidate)) {
            std::ifstream f(candidate, std::ios::binary);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                const std::string s = ss.str();
                return std::vector<u8>(s.begin(), s.end());
            }
        }
        p = p.parent_path();
    }

    std::cerr << "[GPUMesher] Failed to load shader '" << shader_name
              << "' (searched: " << rel_path << " and CWD ancestors)" << std::endl;
    return {};
}

} // namespace

GPUMesher& GPUMesher::Get() {
    static GPUMesher instance;
    return instance;
}

void GPUMesher::Initialize(rhi::RHIDeviceBase* device) {
    if (device_ != nullptr) return;  // already initialized
    if (device == nullptr) return;
    device_ = device;
    // Pipelines created lazily on first GenerateSurfaceNets call.
}

void GPUMesher::Shutdown() {
    DestroyPipelines();
    device_ = nullptr;
}

void GPUMesher::CreatePipelines() {
    if (pipelines_created_ || !device_) return;

    using namespace rhi;

    // ---- Descriptor set layouts (one per kernel binding signature) ----
    auto make_set_layout = [&](const DescriptorSetLayoutBinding* bindings, u32 count) -> DescriptorSetLayoutHandle {
        DescriptorSetLayoutDesc desc{};
        desc.bindingCount = count;
        desc.bindings = bindings;
        return device_->CreateDescriptorSetLayout(desc);
    };

    // classify_cells: uniforms(0), scalar(1), dual_id(2), counters(3)
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        classify_set_layout_ = make_set_layout(b, 4);
    }
    // emit_vertices: uniforms(0), scalar(1), dual_id(2), positions(3), elements(4)
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        emit_vertices_set_layout_ = make_set_layout(b, 5);
    }
    // emit_faces_x/y/z: uniforms(0), scalar(1), dual_id(2), indices(3), counters(4)
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        emit_faces_set_layout_ = make_set_layout(b, 5);
    }
    // write_indirect_args: uniforms(0), counters(1), indirect_args(2)
    {
        DescriptorSetLayoutBinding b[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        write_indirect_set_layout_ = make_set_layout(b, 3);
    }

    // ---- Pipeline layouts ----
    auto make_pipe_layout = [&](DescriptorSetLayoutHandle setLayout) -> PipelineLayoutHandle {
        PipelineLayoutDesc desc{};
        desc.setLayoutCount = 1;
        desc.setLayouts = &setLayout;
        return device_->CreatePipelineLayout(desc);
    };
    classify_layout_      = make_pipe_layout(classify_set_layout_);
    emit_vertices_layout_ = make_pipe_layout(emit_vertices_set_layout_);
    emit_faces_layout_    = make_pipe_layout(emit_faces_set_layout_);
    write_indirect_layout_ = make_pipe_layout(write_indirect_set_layout_);

    // ---- Shader source + pipelines ----
    auto src = LoadShaderSource("SurfaceNetsGPU");
    if (src.empty()) {
        std::cerr << "[GPUMesher] SurfaceNetsGPU.metal not found\n";
        return;
    }

    // Compile-once-per-entry-point. Each ShaderHandle is a separate RHI allocation
    // (Metal compiles each entry point into a distinct MTLFunction) and must be
    // destroyed in DestroyPipelines — pipelines reference shaders but don't own them.
    auto make_compute = [&](const char* entry, PipelineLayoutHandle layout,
                            ShaderHandle& out_shader) -> PipelineHandle {
        ShaderHandle shader = device_->CreateShader(src.data(), src.size(),
                                                    ShaderStage::Compute, entry);
        if (shader == handles::INVALID_SHADER) return handles::INVALID_PIPELINE;
        ComputePipelineDesc desc{};
        desc.computeShader = shader;
        desc.layout = layout;
        desc.threadGroupSize = math::u32v3{4, 4, 4};
        out_shader = shader;
        return device_->CreateComputePipeline(desc);
    };

    classify_pipeline_        = make_compute("classify_cells",        classify_layout_,        classify_shader_);
    emit_vertices_pipeline_   = make_compute("emit_vertices",         emit_vertices_layout_,   emit_vertices_shader_);
    emit_faces_x_pipeline_    = make_compute("emit_faces_x",          emit_faces_layout_,      emit_faces_x_shader_);
    emit_faces_y_pipeline_    = make_compute("emit_faces_y",          emit_faces_layout_,      emit_faces_y_shader_);
    emit_faces_z_pipeline_    = make_compute("emit_faces_z",          emit_faces_layout_,      emit_faces_z_shader_);
    write_indirect_pipeline_  = make_compute("write_indirect_args",   write_indirect_layout_,  write_indirect_shader_);

    pipelines_created_ =
        classify_set_layout_        != handles::INVALID_DESCRIPTOR_SET_LAYOUT &&
        emit_vertices_set_layout_   != handles::INVALID_DESCRIPTOR_SET_LAYOUT &&
        emit_faces_set_layout_      != handles::INVALID_DESCRIPTOR_SET_LAYOUT &&
        write_indirect_set_layout_  != handles::INVALID_DESCRIPTOR_SET_LAYOUT &&
        classify_layout_            != handles::INVALID_PIPELINE_LAYOUT &&
        emit_vertices_layout_       != handles::INVALID_PIPELINE_LAYOUT &&
        emit_faces_layout_          != handles::INVALID_PIPELINE_LAYOUT &&
        write_indirect_layout_      != handles::INVALID_PIPELINE_LAYOUT &&
        classify_pipeline_          != handles::INVALID_PIPELINE &&
        emit_vertices_pipeline_     != handles::INVALID_PIPELINE &&
        emit_faces_x_pipeline_      != handles::INVALID_PIPELINE &&
        emit_faces_y_pipeline_      != handles::INVALID_PIPELINE &&
        emit_faces_z_pipeline_      != handles::INVALID_PIPELINE &&
        write_indirect_pipeline_    != handles::INVALID_PIPELINE;

    if (!pipelines_created_) {
        std::cerr << "[GPUMesher] Pipeline creation partially failed; destroying\n";
        DestroyPipelines();
    }
}

void GPUMesher::DestroyPipelines() {
    if (!device_) return;

    if (classify_pipeline_ != rhi::handles::INVALID_PIPELINE)        device_->DestroyPipeline(classify_pipeline_);
    if (emit_vertices_pipeline_ != rhi::handles::INVALID_PIPELINE)   device_->DestroyPipeline(emit_vertices_pipeline_);
    if (emit_faces_x_pipeline_ != rhi::handles::INVALID_PIPELINE)    device_->DestroyPipeline(emit_faces_x_pipeline_);
    if (emit_faces_y_pipeline_ != rhi::handles::INVALID_PIPELINE)    device_->DestroyPipeline(emit_faces_y_pipeline_);
    if (emit_faces_z_pipeline_ != rhi::handles::INVALID_PIPELINE)    device_->DestroyPipeline(emit_faces_z_pipeline_);
    if (write_indirect_pipeline_ != rhi::handles::INVALID_PIPELINE)  device_->DestroyPipeline(write_indirect_pipeline_);

    if (classify_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT)      device_->DestroyPipelineLayout(classify_layout_);
    if (emit_vertices_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(emit_vertices_layout_);
    if (emit_faces_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT)    device_->DestroyPipelineLayout(emit_faces_layout_);
    if (write_indirect_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(write_indirect_layout_);

    if (classify_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)        device_->DestroyDescriptorSetLayout(classify_set_layout_);
    if (emit_vertices_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)   device_->DestroyDescriptorSetLayout(emit_vertices_set_layout_);
    if (emit_faces_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)      device_->DestroyDescriptorSetLayout(emit_faces_set_layout_);
    if (write_indirect_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT)  device_->DestroyDescriptorSetLayout(write_indirect_set_layout_);

    // Pipelines don't own shaders — destroy after pipelines.
    if (classify_shader_ != rhi::handles::INVALID_SHADER)        device_->DestroyShader(classify_shader_);
    if (emit_vertices_shader_ != rhi::handles::INVALID_SHADER)   device_->DestroyShader(emit_vertices_shader_);
    if (emit_faces_x_shader_ != rhi::handles::INVALID_SHADER)    device_->DestroyShader(emit_faces_x_shader_);
    if (emit_faces_y_shader_ != rhi::handles::INVALID_SHADER)    device_->DestroyShader(emit_faces_y_shader_);
    if (emit_faces_z_shader_ != rhi::handles::INVALID_SHADER)    device_->DestroyShader(emit_faces_z_shader_);
    if (write_indirect_shader_ != rhi::handles::INVALID_SHADER)  device_->DestroyShader(write_indirect_shader_);

    classify_pipeline_        = rhi::handles::INVALID_PIPELINE;
    emit_vertices_pipeline_   = rhi::handles::INVALID_PIPELINE;
    emit_faces_x_pipeline_    = rhi::handles::INVALID_PIPELINE;
    emit_faces_y_pipeline_    = rhi::handles::INVALID_PIPELINE;
    emit_faces_z_pipeline_    = rhi::handles::INVALID_PIPELINE;
    write_indirect_pipeline_  = rhi::handles::INVALID_PIPELINE;
    classify_shader_        = rhi::handles::INVALID_SHADER;
    emit_vertices_shader_   = rhi::handles::INVALID_SHADER;
    emit_faces_x_shader_    = rhi::handles::INVALID_SHADER;
    emit_faces_y_shader_    = rhi::handles::INVALID_SHADER;
    emit_faces_z_shader_    = rhi::handles::INVALID_SHADER;
    write_indirect_shader_  = rhi::handles::INVALID_SHADER;
    classify_layout_      = rhi::handles::INVALID_PIPELINE_LAYOUT;
    emit_vertices_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    emit_faces_layout_    = rhi::handles::INVALID_PIPELINE_LAYOUT;
    write_indirect_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    classify_set_layout_        = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    emit_vertices_set_layout_   = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    emit_faces_set_layout_      = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    write_indirect_set_layout_  = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    pipelines_created_ = false;
}

MarchingCubesResult GPUMesher::GenerateSurfaceNets(
    const PCGField& field,
    const math::v3& bounds_min,
    const math::v3& bounds_max,
    u32 resolution,
    f32 iso_value)
{
    MarchingCubesResult empty;
    if (!IsReady()) return empty;
    if (!pipelines_created_) CreatePipelines();
    if (!pipelines_created_) return empty;
    if (resolution < 2 || resolution > 256) return empty;

    const math::v3 extent{
        bounds_max.x - bounds_min.x,
        bounds_max.y - bounds_min.y,
        bounds_max.z - bounds_min.z,
    };
    if (extent.x <= 0.0f || extent.y <= 0.0f || extent.z <= 0.0f) return empty;

    const math::v3 voxel{
        extent.x / static_cast<f32>(resolution),
        extent.y / static_cast<f32>(resolution),
        extent.z / static_cast<f32>(resolution),
    };

    const u32 n  = resolution + 1;
    const u32 n2 = n * n;
    const u32 n3 = n * n * n;
    const u32 res3 = resolution * resolution * resolution;

    // Pack uniforms
    SurfaceNetsUniforms uni{};
    uni.resolution = resolution;
    uni.n = n;
    uni.n2 = n2;
    uni.voxel_x = voxel.x; uni.voxel_y = voxel.y; uni.voxel_z = voxel.z;
    uni.origin_x = bounds_min.x; uni.origin_y = bounds_min.y; uni.origin_z = bounds_min.z;
    uni.extent_x = extent.x; uni.extent_y = extent.y; uni.extent_z = extent.z;
    uni.iso_value = iso_value;

    ScratchBuffers scratch;

    // Allocate buffers (worst-case sizes per spec §4.2)
    // Dynamic (Shared storage on Metal) so we can MapBuffer for synchronous readback.
    // Static would give us StorageModePrivate → contents() == nullptr → MapBuffer fails.
    auto make_storage_buf = [&](u64 bytes) -> rhi::ResourceHandle {
        rhi::BufferDesc desc{};
        desc.size = bytes;
        desc.bindFlags = (u32)rhi::BufferUsageFlags::Storage;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        return device_->CreateBuffer(desc);
    };

    // Uniform buffer (Dynamic so we can UpdateBufferData)
    {
        rhi::BufferDesc desc{};
        desc.size = sizeof(SurfaceNetsUniforms);
        desc.bindFlags = (u32)rhi::BufferUsageFlags::Uniform;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        scratch.uniforms = device_->CreateBuffer(desc);
    }
    // scalar_volume: Storage + TransferDst for upload
    {
        rhi::BufferDesc desc{};
        desc.size = sizeof(f32) * n3;
        desc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::TransferDst);
        desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        desc.usage = rhi::GPUMemoryUsage::Static;
        scratch.scalar_volume = device_->CreateBuffer(desc);
    }
    scratch.dual_id   = make_storage_buf(sizeof(u32) * res3);
    scratch.positions = make_storage_buf(sizeof(f32) * 3 * res3);
    scratch.elements  = make_storage_buf(20u * res3);
    // emit_faces iterates grid vertices (n³ ≈ res³); each can emit up to 6
    // triangles (2 per axis × 3 axes) when all adjacent cells straddle. Worst
    // case = 6 tris × 3 idx = 18 idx per grid vertex. res³ bound is tight.
    scratch.indices   = make_storage_buf(sizeof(u32) * 18 * res3);
    // counters: Dynamic so we can UpdateBufferData (zero init)
    {
        rhi::BufferDesc desc{};
        desc.size = sizeof(u32) * 2;
        desc.bindFlags = (u32)rhi::BufferUsageFlags::Storage;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        scratch.counters = device_->CreateBuffer(desc);
    }
    // indirect_args: Storage + Indirect
    {
        rhi::BufferDesc desc{};
        desc.size = 16u;
        desc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Indirect);
        desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        desc.usage = rhi::GPUMemoryUsage::Static;
        scratch.indirect_args = device_->CreateBuffer(desc);
    }

    if (scratch.uniforms       == rhi::handles::INVALID_RESOURCE ||
        scratch.scalar_volume  == rhi::handles::INVALID_RESOURCE ||
        scratch.dual_id        == rhi::handles::INVALID_RESOURCE ||
        scratch.positions      == rhi::handles::INVALID_RESOURCE ||
        scratch.elements       == rhi::handles::INVALID_RESOURCE ||
        scratch.indices        == rhi::handles::INVALID_RESOURCE ||
        scratch.counters       == rhi::handles::INVALID_RESOURCE ||
        scratch.indirect_args  == rhi::handles::INVALID_RESOURCE) {
        DestroyScratch(device_, scratch);
        return empty;
    }

    // Pass 0: CPU sample loop
    std::vector<f32> scalar(n3);
    for (u32 k = 0; k < n; ++k) {
        for (u32 j = 0; j < n; ++j) {
            for (u32 i = 0; i < n; ++i) {
                math::v3 p{
                    bounds_min.x + voxel.x * static_cast<f32>(i),
                    bounds_min.y + voxel.y * static_cast<f32>(j),
                    bounds_min.z + voxel.z * static_cast<f32>(k),
                };
                scalar[i + n * j + n2 * k] = field.SampleFloat(p);
            }
        }
    }
    device_->UpdateBufferData(scratch.scalar_volume, scalar.data(), scalar.size() * sizeof(f32));
    device_->UpdateBufferData(scratch.uniforms, &uni, sizeof(uni));

    // Zero counters
    u32 zero_counters[2] = {0u, 0u};
    device_->UpdateBufferData(scratch.counters, zero_counters, sizeof(zero_counters));

    using namespace rhi;

    // ---- Create 4 descriptor sets (one per kernel signature) ----
    auto make_ds = [&](DescriptorSetLayoutHandle set_layout,
                       const DescriptorBufferInfo* infos,
                       const DescriptorType* types,
                       u32 count) -> DescriptorSetHandle {
        DescriptorSetHandle ds = device_->CreateDescriptorSet({set_layout});
        if (ds == handles::INVALID_DESCRIPTOR_SET) return ds;
        // Build writes with per-binding info. We use stack arrays sized to the max binding count.
        WriteDescriptorSet writes[8];
        for (u32 i = 0; i < count; ++i) {
            writes[i] = {ds, i, 0, 1, types[i], nullptr, &infos[i]};
        }
        device_->UpdateDescriptorSets(count, writes);
        return ds;
    };

    DescriptorSetHandle classify_ds      = handles::INVALID_DESCRIPTOR_SET;
    DescriptorSetHandle emit_vertices_ds = handles::INVALID_DESCRIPTOR_SET;
    DescriptorSetHandle emit_faces_ds    = handles::INVALID_DESCRIPTOR_SET;
    DescriptorSetHandle write_indirect_ds = handles::INVALID_DESCRIPTOR_SET;
    {
        DescriptorBufferInfo infos[4] = {
            {scratch.uniforms,      0, 0},
            {scratch.scalar_volume, 0, 0},
            {scratch.dual_id,       0, 0},
            {scratch.counters,      0, 0},
        };
        DescriptorType types[4] = {
            DescriptorType::UniformBuffer,
            DescriptorType::StorageBuffer,
            DescriptorType::StorageBuffer,
            DescriptorType::StorageBuffer,
        };
        classify_ds = make_ds(classify_set_layout_, infos, types, 4);
    }
    {
        DescriptorBufferInfo infos[5] = {
            {scratch.uniforms,      0, 0},
            {scratch.scalar_volume, 0, 0},
            {scratch.dual_id,       0, 0},
            {scratch.positions,     0, 0},
            {scratch.elements,      0, 0},
        };
        DescriptorType types[5] = {
            DescriptorType::UniformBuffer,
            DescriptorType::StorageBuffer, DescriptorType::StorageBuffer,
            DescriptorType::StorageBuffer, DescriptorType::StorageBuffer,
        };
        emit_vertices_ds = make_ds(emit_vertices_set_layout_, infos, types, 5);
    }
    {
        DescriptorBufferInfo infos[5] = {
            {scratch.uniforms,      0, 0},
            {scratch.scalar_volume, 0, 0},
            {scratch.dual_id,       0, 0},
            {scratch.indices,       0, 0},
            {scratch.counters,      0, 0},
        };
        DescriptorType types[5] = {
            DescriptorType::UniformBuffer,
            DescriptorType::StorageBuffer, DescriptorType::StorageBuffer,
            DescriptorType::StorageBuffer, DescriptorType::StorageBuffer,
        };
        emit_faces_ds = make_ds(emit_faces_set_layout_, infos, types, 5);
    }
    {
        DescriptorBufferInfo infos[3] = {
            {scratch.uniforms,      0, 0},
            {scratch.counters,      0, 0},
            {scratch.indirect_args, 0, 0},
        };
        DescriptorType types[3] = {
            DescriptorType::UniformBuffer,
            DescriptorType::StorageBuffer,
            DescriptorType::StorageBuffer,
        };
        write_indirect_ds = make_ds(write_indirect_set_layout_, infos, types, 3);
    }

    if (classify_ds       == handles::INVALID_DESCRIPTOR_SET ||
        emit_vertices_ds  == handles::INVALID_DESCRIPTOR_SET ||
        emit_faces_ds     == handles::INVALID_DESCRIPTOR_SET ||
        write_indirect_ds == handles::INVALID_DESCRIPTOR_SET) {
        if (classify_ds       != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(classify_ds);
        if (emit_vertices_ds  != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(emit_vertices_ds);
        if (emit_faces_ds     != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(emit_faces_ds);
        if (write_indirect_ds != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(write_indirect_ds);
        DestroyScratch(device_, scratch);
        return empty;
    }

    // ---- Command buffer + dispatch ----
    CommandBufferHandle cmd_handle = device_->CreateCommandBuffer(CommandQueueType::Compute);
    if (cmd_handle == handles::INVALID_COMMAND_BUFFER) {
        device_->DestroyDescriptorSet(classify_ds);
        device_->DestroyDescriptorSet(emit_vertices_ds);
        device_->DestroyDescriptorSet(emit_faces_ds);
        device_->DestroyDescriptorSet(write_indirect_ds);
        DestroyScratch(device_, scratch);
        return empty;
    }
    RHICommandBuffer* cmd = GetCommandBuffer(cmd_handle);

    cmd->Begin();

    const u32 res_groups = (resolution + 3) / 4;
    const u32 n_groups    = (n + 3) / 4;

    // Pass 1: classify_cells
    {
        cmd->BindComputePipeline(classify_pipeline_);
        DescriptorSetHandle ds = classify_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, classify_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(res_groups, res_groups, res_groups);
    }
    // Pass 2: emit_vertices
    {
        cmd->BindComputePipeline(emit_vertices_pipeline_);
        DescriptorSetHandle ds = emit_vertices_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, emit_vertices_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(res_groups, res_groups, res_groups);
    }
    // Pass 3: emit_faces_x / y / z (share emit_faces_ds)
    {
        cmd->BindComputePipeline(emit_faces_x_pipeline_);
        DescriptorSetHandle ds = emit_faces_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, emit_faces_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(n_groups, n_groups, n_groups);
    }
    {
        cmd->BindComputePipeline(emit_faces_y_pipeline_);
        DescriptorSetHandle ds = emit_faces_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, emit_faces_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(n_groups, n_groups, n_groups);
    }
    {
        cmd->BindComputePipeline(emit_faces_z_pipeline_);
        DescriptorSetHandle ds = emit_faces_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, emit_faces_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(n_groups, n_groups, n_groups);
    }
    // Pass 4: write_indirect_args
    {
        cmd->BindComputePipeline(write_indirect_pipeline_);
        DescriptorSetHandle ds = write_indirect_ds;
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, write_indirect_layout_, 0, 1, &ds, 0, nullptr);
        cmd->Dispatch(1, 1, 1);
    }

    cmd->End();

    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd_handle;
    device_->Submit(submit);
    cmd->WaitForCompletion();  // synchronous readback (spec §5) — scopes stall to this cmd buffer

    // ---- Readback ----
    MarchingCubesResult result;

    // Read counters (u32[2]) to size the position/index readback.
    u32 counters[2] = {0u, 0u};
    if (void* mapped = device_->MapBuffer(scratch.counters, 0, sizeof(counters))) {
        std::memcpy(counters, mapped, sizeof(counters));
        device_->UnmapBuffer(scratch.counters);
    }
    const u32 vert_count = counters[0];
    const u32 idx_count  = counters[1];

    if (vert_count == 0u || idx_count == 0u || vert_count > res3 || idx_count > 18u * res3) {
        // Empty surface (e.g. iso_value out of range) or corrupted counters.
        device_->DestroyDescriptorSet(classify_ds);
        device_->DestroyDescriptorSet(emit_vertices_ds);
        device_->DestroyDescriptorSet(emit_faces_ds);
        device_->DestroyDescriptorSet(write_indirect_ds);
        device_->DestroyCommandBuffer(cmd_handle);
        DestroyScratch(device_, scratch);
        return empty;
    }

    result.positions.resize(vert_count * 3);
    result.normals.resize  (vert_count * 3);
    result.uvs.resize      (vert_count * 2);
    result.indices.resize  (idx_count);

    bool readback_ok = true;
    // Read positions.
    if (void* mapped = device_->MapBuffer(scratch.positions, 0, sizeof(f32) * 3 * vert_count)) {
        std::memcpy(result.positions.data(), mapped, sizeof(f32) * 3 * vert_count);
        device_->UnmapBuffer(scratch.positions);
    } else {
        std::cerr << "[GPUMesher] MapBuffer failed for positions\n";
        readback_ok = false;
    }

    // Read elements (20B each), unpack into normals + uvs.
    constexpr f32 INV_INTERVALS = 2.0f / 65535.0f;
    {
        std::vector<u8> elems(20u * vert_count);
        if (void* mapped = device_->MapBuffer(scratch.elements, 0, 20u * vert_count)) {
            std::memcpy(elems.data(), mapped, 20u * vert_count);
            device_->UnmapBuffer(scratch.elements);
        } else {
            std::cerr << "[GPUMesher] MapBuffer failed for elements\n";
            readback_ok = false;
        }
        for (u32 v = 0; v < vert_count; ++v) {
            const u8* p = elems.data() + v * 20u;
            u16 n0, n1;
            std::memcpy(&n0, p + 4, 2);
            std::memcpy(&n1, p + 6, 2);
            const u8 sign_byte = p[3];  // top byte of ColorTSign
            const f32 nx = static_cast<f32>(n0) * INV_INTERVALS - 1.f;
            const f32 ny = static_cast<f32>(n1) * INV_INTERVALS - 1.f;
            const f32 nz_sign = (sign_byte & 0x02) ? 1.f : -1.f;
            const f32 nz_sq = std::max(0.f, 1.f - nx * nx - ny * ny);
            const f32 nz = std::copysign(std::sqrt(nz_sq), nz_sign);
            result.normals[v * 3 + 0] = nx;
            result.normals[v * 3 + 1] = ny;
            result.normals[v * 3 + 2] = nz;
            std::memcpy(&result.uvs[v * 2 + 0], p + 12, 4);
            std::memcpy(&result.uvs[v * 2 + 1], p + 16, 4);
        }
    }

    // Read indices.
    if (void* mapped = device_->MapBuffer(scratch.indices, 0, sizeof(u32) * idx_count)) {
        std::memcpy(result.indices.data(), mapped, sizeof(u32) * idx_count);
        device_->UnmapBuffer(scratch.indices);
    } else {
        std::cerr << "[GPUMesher] MapBuffer failed for indices\n";
        readback_ok = false;
    }

    if (!readback_ok) {
        std::cerr << "[GPUMesher] GPU readback partially failed; returning empty result\n";
        device_->DestroyDescriptorSet(classify_ds);
        device_->DestroyDescriptorSet(emit_vertices_ds);
        device_->DestroyDescriptorSet(emit_faces_ds);
        device_->DestroyDescriptorSet(write_indirect_ds);
        device_->DestroyCommandBuffer(cmd_handle);
        DestroyScratch(device_, scratch);
        return empty;
    }

    // ---- Cleanup ----
    device_->DestroyDescriptorSet(classify_ds);
    device_->DestroyDescriptorSet(emit_vertices_ds);
    device_->DestroyDescriptorSet(emit_faces_ds);
    device_->DestroyDescriptorSet(write_indirect_ds);
    device_->DestroyCommandBuffer(cmd_handle);
    DestroyScratch(device_, scratch);

    return result;
}

} // namespace primal::graphics::pcg
