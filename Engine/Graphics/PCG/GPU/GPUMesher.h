#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/PCG/MarchingCubes.h"

namespace primal::graphics::pcg {

// Singleton owning the RHI device pointer and cached compute pipelines for GPU
// meshing. Lifetime bound to StandardRenderPipeline (Initialize on pipeline init,
// Shutdown on pipeline shutdown). PCG nodes access via Get(); if IsReady() is
// false (test environment without RHI device), callers must fall back to CPU.
//
// Mirrors the GlobalSDF::Get() pattern so PCGNode::Execute() can reach the device
// without its signature changing.
class GPUMesher {
public:
    static GPUMesher& Get();

    // Called by StandardRenderPipeline::InitializeSubsystems after the RHI device
    // is ready. Idempotent — safe to call twice.
    void Initialize(rhi::RHIDeviceBase* device);

    // Called by StandardRenderPipeline::ShutdownSubsystems. Frees all cached
    // pipelines and buffers. Safe to call without Initialize.
    void Shutdown();

    bool IsReady() const { return device_ != nullptr; }

    // Run SurfaceNets on the GPU. Reads `field` on CPU (Pass 0), uploads scalar
    // volume, dispatches 4 compute passes, blocking-reads back vertex/index
    // buffers, returns MarchingCubesResult identical in shape to the CPU kernel.
    //
    // Returns empty result if !IsReady() (caller should fall back to CPU).
    MarchingCubesResult GenerateSurfaceNets(
        const PCGField& field,
        const math::v3& bounds_min,
        const math::v3& bounds_max,
        u32 resolution,
        f32 iso_value);

private:
    GPUMesher() = default;
    ~GPUMesher() = default;
    GPUMesher(const GPUMesher&) = delete;
    GPUMesher& operator=(const GPUMesher&) = delete;

    rhi::RHIDeviceBase* device_{nullptr};

    // Pipeline state — populated lazily by CreatePipelines() on first
    // GenerateSurfaceNets call. Destroyed by DestroyPipelines() in Shutdown.
    bool pipelines_created_{false};
    void CreatePipelines();
    void DestroyPipelines();

    // 4 descriptor set layouts — one per kernel binding signature.
    // Slots 3/4 conflict across kernels (positions vs indices vs counters),
    // so we cannot share one layout.
    rhi::DescriptorSetLayoutHandle classify_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle emit_vertices_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle emit_faces_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetLayoutHandle write_indirect_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};

    // 4 pipeline layouts (one per descriptor set layout).
    rhi::PipelineLayoutHandle classify_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle emit_vertices_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle emit_faces_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::PipelineLayoutHandle write_indirect_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    // 6 compute pipelines (emit_faces has 3 entry points sharing one layout).
    rhi::PipelineHandle classify_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle emit_vertices_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle emit_faces_x_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle emit_faces_y_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle emit_faces_z_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineHandle write_indirect_pipeline_{rhi::handles::INVALID_PIPELINE};

    // 6 compiled shader handles (CreateShader returns one per entry point;
    // pipelines reference these but do not own them — must be destroyed here).
    rhi::ShaderHandle classify_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle emit_vertices_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle emit_faces_x_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle emit_faces_y_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle emit_faces_z_shader_{rhi::handles::INVALID_SHADER};
    rhi::ShaderHandle write_indirect_shader_{rhi::handles::INVALID_SHADER};
};

} // namespace primal::graphics::pcg
