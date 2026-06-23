#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/PCG/MarchingCubes.h"

namespace primal::graphics::nanite { class GlobalSDF; }
namespace primal::graphics { struct StreamingMesh; }

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

    // Expose the device pointer so PCG node lifecycle code (CreateStreamingMesh,
    // buffer mapping for counter readback) can reach the RHI without a second
    // singleton lookup. Returns nullptr when !IsReady().
    rhi::RHIDeviceBase* GetDevice() const { return device_; }

    // Queue a buffer handle for destruction at the next frame boundary.
    // Used by StreamingMesh owners that may die mid-frame (see spec §7.2).
    void EnqueueDeferredDestroy(rhi::ResourceHandle handle);

    // Free all queued handles. Called by StandardRenderPipeline at frame end.
    // On Metal this is safe even while command buffers are in-flight because
    // Metal retains resources referenced by encoders. A Vulkan/D3D12 backend
    // would need a GPU fence wait before calling this.
    void DrainDeferredDestroys();

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

    // Run SurfaceNets directly on GlobalSDF cascade textures. Skips Pass 0
    // (no CPU sample loop, no scalar upload). Writes into the caller-owned
    // `target` StreamingMesh buffers. Returns true on successful dispatch.
    //
    // Caller responsibility: after return, read back target.counters
    // (8 bytes) for vertex/index counts and call
    // PipelineUpdateStreamingMeshEntity. Counters are zeroed internally.
    bool GenerateSurfaceNetsFromGlobalSDF(
        const primal::graphics::nanite::GlobalSDF& sdf,
        const math::v3& bounds_min,
        const math::v3& bounds_max,
        u32 resolution,
        f32 iso_value,
        primal::graphics::StreamingMesh& target);

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

    // --- SDF-variant pipeline state (Phase 9.3b) ---
    // classify_cells_sdf has 3 extra texture bindings vs classify_cells, so it
    // needs its own descriptor set layout. Passes 2-4 reuse the 9.3a pipelines
    // since the shaders are byte-identical.
    rhi::DescriptorSetLayoutHandle classify_sdf_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineLayoutHandle      classify_sdf_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::ShaderHandle              classify_sdf_shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineHandle            classify_sdf_pipeline_{rhi::handles::INVALID_PIPELINE};
    bool sdf_pipelines_created_{false};

    void CreateSDFPipelines();
    void DestroySDFPipelines();

    // Handles queued by StreamingMesh owners that may die mid-frame.
    // Freed by DrainDeferredDestroys() at the next frame boundary. On Metal,
    // in-flight command buffers retain their referenced resources, so the
    // queued handles are safe to release without an explicit GPU fence wait
    // (spec §7.2). A Vulkan/D3D12 backend would need to wait on a fence first.
    utl::vector<rhi::ResourceHandle> deferred_destroy_queue_;
};

} // namespace primal::graphics::pcg
