#include "Graphics/PCG/GPU/GPUMesher.h"

namespace primal::graphics::pcg {

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
    // TODO(Task 14): release pipeline handles, descriptor set layouts.
    pipelines_created_ = false;
    device_ = nullptr;
}

void GPUMesher::CreatePipelines() {
    // TODO(Task 14): real implementation.
    pipelines_created_ = true;
}

MarchingCubesResult GPUMesher::GenerateSurfaceNets(
    const PCGField& /*field*/,
    const math::v3& /*bounds_min*/,
    const math::v3& /*bounds_max*/,
    u32 /*resolution*/,
    f32 /*iso_value*/) {
    // TODO(Task 15): full GPU implementation. Stub returns empty so the
    // dispatch in MarchingCubesNode falls through to CPU until GPU is online.
    MarchingCubesResult empty;
    return empty;
}

} // namespace primal::graphics::pcg
