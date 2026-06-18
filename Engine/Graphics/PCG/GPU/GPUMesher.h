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

    // Pipeline state — populated lazily on first GenerateSurfaceNets call.
    bool pipelines_created_{false};
    void CreatePipelines();  // defined in GPUMesher.cpp; full impl lands in Task 14
};

} // namespace primal::graphics::pcg
