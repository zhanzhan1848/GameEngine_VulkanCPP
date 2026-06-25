#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics {

// Persistent GPU buffer bundle for a streaming mesh entity. Allocated once at
// node creation; reused across re-executes (only the counters buffer is zeroed
// per Execute). Worst-case sizing assumes (res+1)^3 vertices and 18 * res^3 indices.
//
// Ownership: the PCG node owns the StreamingMesh (alloc in Initialize, free in
// destructor). Destruction is deferred through GPUMesher's queue to avoid
// use-after-free when the node dies mid-frame (see section 7.2 of the spec).
struct StreamingMesh {
    rhi::ResourceHandle positions    {rhi::handles::INVALID_RESOURCE}; // f32 x 3 x max_verts
    rhi::ResourceHandle elements     {rhi::handles::INVALID_RESOURCE}; // 20B x max_verts
    rhi::ResourceHandle indices      {rhi::handles::INVALID_RESOURCE}; // u32 x max_indices
    rhi::ResourceHandle counters     {rhi::handles::INVALID_RESOURCE}; // u32[2] {vert, idx}
    rhi::ResourceHandle indirect_args{rhi::handles::INVALID_RESOURCE}; // MTLDrawPrimitivesIndirectCommand
    u32     max_verts   {0};
    u32     max_indices {0};
    math::v3 bounds_min{};
    math::v3 bounds_max{};
    u64     generation  {0};                   // bumped each Execute; RenderScene skips stale
    id::id_type entity_id{id::invalid_id};     // set by PipelineRegisterStreamingMeshEntity

    bool IsValid() const {
        return positions     != rhi::handles::INVALID_RESOURCE
            && elements      != rhi::handles::INVALID_RESOURCE
            && indices       != rhi::handles::INVALID_RESOURCE
            && counters      != rhi::handles::INVALID_RESOURCE
            && indirect_args != rhi::handles::INVALID_RESOURCE;
    }
};

// Allocates all 5 buffers for worst-case capacity at the given resolution.
// Returns StreamingMesh with IsValid()==false on partial failure (caller must
// still call DestroyStreamingMesh to free partial allocs).
// Resolution is capped at 256 (matches GPUMesher::GenerateSurfaceNets cap).
StreamingMesh CreateStreamingMesh(
    rhi::RHIDeviceBase* device,
    u32 resolution,
    const math::v3& bounds_min,
    const math::v3& bounds_max);

// Frees all non-invalid buffers. Safe to call on an uninitialized struct.
// Does NOT unregister the entity - caller must do that first.
void DestroyStreamingMesh(rhi::RHIDeviceBase* device, StreamingMesh& sm);

} // namespace primal::graphics
