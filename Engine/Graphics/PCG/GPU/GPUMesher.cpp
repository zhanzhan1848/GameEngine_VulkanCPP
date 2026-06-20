#include "Graphics/PCG/GPU/GPUMesher.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "Graphics/RHI/Core/RHIDevice.h"
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

    auto make_compute = [&](const char* entry, PipelineLayoutHandle layout) -> PipelineHandle {
        ShaderHandle shader = device_->CreateShader(src.data(), src.size(),
                                                    ShaderStage::Compute, entry);
        if (shader == handles::INVALID_SHADER) return handles::INVALID_PIPELINE;
        ComputePipelineDesc desc{};
        desc.computeShader = shader;
        desc.layout = layout;
        desc.threadGroupSize = math::u32v3{4, 4, 4};
        return device_->CreateComputePipeline(desc);
    };

    classify_pipeline_        = make_compute("classify_cells",        classify_layout_);
    emit_vertices_pipeline_   = make_compute("emit_vertices",         emit_vertices_layout_);
    emit_faces_x_pipeline_    = make_compute("emit_faces_x",          emit_faces_layout_);
    emit_faces_y_pipeline_    = make_compute("emit_faces_y",          emit_faces_layout_);
    emit_faces_z_pipeline_    = make_compute("emit_faces_z",          emit_faces_layout_);
    write_indirect_pipeline_  = make_compute("write_indirect_args",   write_indirect_layout_);

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

    classify_pipeline_        = rhi::handles::INVALID_PIPELINE;
    emit_vertices_pipeline_   = rhi::handles::INVALID_PIPELINE;
    emit_faces_x_pipeline_    = rhi::handles::INVALID_PIPELINE;
    emit_faces_y_pipeline_    = rhi::handles::INVALID_PIPELINE;
    emit_faces_z_pipeline_    = rhi::handles::INVALID_PIPELINE;
    write_indirect_pipeline_  = rhi::handles::INVALID_PIPELINE;
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
    if (!pipelines_created_) return empty;  // shader compile / pipeline creation failed
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
    auto make_storage_buf = [&](u64 bytes) -> rhi::ResourceHandle {
        rhi::BufferDesc desc{};
        desc.size = bytes;
        desc.bindFlags = (u32)rhi::BufferUsageFlags::Storage;
        desc.memoryUsage = rhi::GPUMemoryUsage::Static;
        desc.usage = rhi::GPUMemoryUsage::Static;
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
    scratch.indices   = make_storage_buf(sizeof(u32) * 18 * res3);  // worst case: 3 axes × 2 tris × 3 idx per straddling cell
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

    // Tasks 14 & 15 will fill in: pipeline creation, dispatch, readback.
    DestroyScratch(device_, scratch);
    return empty;
}

} // namespace primal::graphics::pcg
