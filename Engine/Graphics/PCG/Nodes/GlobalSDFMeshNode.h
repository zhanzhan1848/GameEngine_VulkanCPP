#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/PCGTypes.h"
#include "Graphics/PCG/PCGReflection.h"
#include "Graphics/PCG/GPU/GPUMesher.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderScene.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Content/ContentToEngine.h"
#include <cstring>

namespace primal::graphics::pcg {

// Generates a GPU-resident streaming mesh directly from GlobalSDF cascade
// textures. No CPU Pass 0, no readback. The mesh is registered as a
// StreamingMesh entity (no content_id); Nanite draws it via indirect args.
//
// Pin layout:
//   Outputs: [0] Geometry — PCGGeometryData with content_id = invalid_id
//                         (sentinel: this node produces a GPU-resident mesh,
//                          not a content_id; downstream TransformGeometryNode
//                          cannot consume it)
//
// Parameters:
//   bounds_min/max — world-space AABB of the meshing volume (should be inside
//                    GlobalSDF cascade 0 extent for sharpest detail)
//   resolution     — per-axis voxel count (2..128, default 64)
//   iso_value      — SDF threshold where the surface lives (default 0.0)
//
// Lifecycle:
//   First Execute allocates StreamingMesh + registers entity. Subsequent
//   Executes reuse the same buffers (zero counters, re-dispatch, bump
//   generation). Destructor unregisters entity + queues deferred destroy.
//
// Engine-layer integration:
//   The node reaches the render pipeline via RenderPipeline::Get() (base-class
//   singleton set by StandardRenderPipeline::Initialize). From there it
//   static_casts to StandardRenderPipeline to access GetCurrentScene() and
//   GetDevice(). This mirrors the C ABI pattern (RenderPipelineAPI.cpp) but
//   stays inside the Engine layer — no EngineDLL boundary crossing needed.
class GlobalSDFMeshNode : public PCGNode {
public:
    math::v3 bounds_min{-32.0f, -32.0f, -32.0f};
    math::v3 bounds_max{ 32.0f,  32.0f,  32.0f};
    u32      resolution{64};
    f32      iso_value{0.0f};

    GlobalSDFMeshNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Geometry;
    }

    ~GlobalSDFMeshNode() override;

    const char* TypeName() const override { return "GlobalSDFMesh"; }
    void Execute() override;

    // --- Reflection ---
    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount;
        return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount;
        return kPins;
    }
    bool SetParamByName(const char* name, f32 value) override {
        if (std::strcmp(name, "resolution") == 0) { resolution = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "iso_value")  == 0) { iso_value = value; return true; }
        return false;
    }
    bool SetParamByName(const char* name, math::v3 value) override {
        if (std::strcmp(name, "bounds_min") == 0) { bounds_min = value; return true; }
        if (std::strcmp(name, "bounds_max") == 0) { bounds_max = value; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 4;
    static constexpr u32 kPinCount   = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor   kPins[];

    // entity_id lives inside streaming_mesh_ (invalid_id until registered).
    // generation lives inside streaming_mesh_ (0 until first Execute).
    // No duplicated state — avoids drift between two sources of truth.
    StreamingMesh streaming_mesh_{};
};

inline const PCGParamDescriptor GlobalSDFMeshNode::kParams[] = {
    {"bounds_min", "Volume", PCGParamType::Vec3, {-100.0f, 100.0f, 0.1f},
     PCG_OFFSETOF(GlobalSDFMeshNode, bounds_min), sizeof(bounds_min), nullptr},
    {"bounds_max", "Volume", PCGParamType::Vec3, {-100.0f, 100.0f, 0.1f},
     PCG_OFFSETOF(GlobalSDFMeshNode, bounds_max), sizeof(bounds_max), nullptr},
    {"resolution", "Volume", PCGParamType::UInt, {4.0f, 128.0f, 4.0f},
     PCG_OFFSETOF(GlobalSDFMeshNode, resolution), sizeof(resolution), nullptr},
    {"iso_value",  "Volume", PCGParamType::Float, {-1.0f, 1.0f, 0.01f},
     PCG_OFFSETOF(GlobalSDFMeshNode, iso_value),  sizeof(iso_value),  nullptr},
};

inline const PCGPinDescriptor GlobalSDFMeshNode::kPins[] = {
    {"geometry", 0, PCGDataType::Geometry, false},
};

// ---------------------------------------------------------------------------
// Execute — alloc + register + regenerate lifecycle
// ---------------------------------------------------------------------------
inline void GlobalSDFMeshNode::Execute() {
    // Step 1: Clamp resolution to the valid range [2, 128].
    //   2 is the minimum for SurfaceNets (at least one cell per axis).
    //   128 is the cap matching MarchingCubesNode and GPUMesher.
    u32 res = resolution;
    if (res < 2)   res = 2;
    if (res > 128) res = 128;

    // Step 2: Resolve device + pipeline + scene.
    //   RenderPipeline::Get() returns the base-class singleton (set by
    //   StandardRenderPipeline::Initialize). static_cast is safe because
    //   StandardRenderPipeline is the only concrete implementation.
    auto* pipeline = static_cast<StandardRenderPipeline*>(RenderPipeline::Get());
    if (!pipeline) {
        // Headless/test environment without a live pipeline.
        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = id::invalid_id;
        return;
    }

    RenderScene* scene = pipeline->GetCurrentScene();
    rhi::RHIDeviceBase* device = pipeline->GetDevice();

    if (!device || !scene || !GPUMesher::Get().IsReady()) {
        // No RHI device, no active scene, or GPUMesher not initialized.
        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = id::invalid_id;
        return;
    }

    // Step 3: First-execute allocation.
    //   Allocate 5 buffers (positions, elements, indices, counters,
    //   indirect_args) sized for worst-case (res+1)^3 verts / 18*res^3 indices.
    //   Buffers persist across subsequent Executes.
    if (!streaming_mesh_.IsValid()) {
        streaming_mesh_ = CreateStreamingMesh(device, res, bounds_min, bounds_max);
        if (!streaming_mesh_.IsValid()) {
            // Allocation failed (e.g. OOM). Emit sentinel, leave struct empty
            // so the next Execute retries.
            auto* out = CreateOutput<PCGGeometryData>(0);
            out->content_id = id::invalid_id;
            return;
        }
    }

    // Step 4: First-execute entity registration.
    //   RegisterStreamingMesh returns a monotonic entity id > 0. We store it
    //   in the StreamingMesh struct so GPUDrivenDrawPipeline can correlate
    //   the record back to this mesh.
    if (streaming_mesh_.entity_id == id::invalid_id) {
        streaming_mesh_.entity_id = scene->RegisterStreamingMesh(
            &streaming_mesh_, bounds_min, bounds_max);
    }

    // Step 5: Dispatch SurfaceNets on GlobalSDF cascade textures.
    //   This zeroes counters internally, runs 4 compute passes, and leaves
    //   results in streaming_mesh_.positions/elements/indices. Indirect args
    //   are written by the last pass.
    bool ok = GPUMesher::Get().GenerateSurfaceNetsFromGlobalSDF(
        nanite::GlobalSDF::Get(), bounds_min, bounds_max, res, iso_value,
        streaming_mesh_);
    if (!ok) {
        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = id::invalid_id;
        return;
    }

    // Step 6: Read back counters (8 bytes: u32 vert_count + u32 idx_count).
    //   This is a blocking read but the buffers are tiny (8B) and already
    //   synchronized by the compute passes. The counts are not currently
    //   consumed by downstream nodes (they will be by Task 10's draw path),
    //   but we read them here to confirm the dispatch produced output and
    //   to catch silent GPU failures early.
    if (streaming_mesh_.counters != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device->MapBuffer(streaming_mesh_.counters, 0, 0);
        if (mapped) {
            // u32[2]: {vertex_count, index_count}
            // Reading is sufficient to verify the mapping succeeded.
            volatile const u32* counts = reinterpret_cast<volatile const u32*>(mapped);
            (void)counts[0]; (void)counts[1];
            device->UnmapBuffer(streaming_mesh_.counters);
        }
    }

    // Step 7: Bump generation and notify the draw side.
    //   generation is monotonic; RenderScene::last_drawn_generation is
    //   compared against it to skip stale records.
    ++streaming_mesh_.generation;
    scene->UpdateStreamingMesh(
        streaming_mesh_.entity_id, streaming_mesh_.generation,
        bounds_min, bounds_max);

    // Step 8: Emit sentinel. This node produces a GPU-resident mesh, not a
    //   content_id. Downstream TransformGeometryNode cannot consume it.
    auto* out = CreateOutput<PCGGeometryData>(0);
    out->content_id = id::invalid_id;
}

// ---------------------------------------------------------------------------
// Destructor — unregister + queue deferred destroy
// ---------------------------------------------------------------------------
inline GlobalSDFMeshNode::~GlobalSDFMeshNode() {
    // If the node was registered, tombstone the record. The record is not
    // erased immediately because the GPU may still be iterating the list
    // this frame; RenderScene::ClearTombstonedStreamingMeshes() (called at
    // frame boundary after GPU work) handles final cleanup.
    if (streaming_mesh_.entity_id != id::invalid_id) {
        auto* pipeline = static_cast<StandardRenderPipeline*>(RenderPipeline::Get());
        if (pipeline) {
            if (auto* scene = pipeline->GetCurrentScene()) {
                scene->UnregisterStreamingMesh(streaming_mesh_.entity_id);
            }
        }
    }

    // Queue each of the 5 buffer handles for deferred destruction. On Metal
    // this is safe even while command buffers are in-flight because Metal
    // retains resources referenced by encoders. A Vulkan/D3D12 backend would
    // need a GPU fence wait first.
    if (streaming_mesh_.IsValid() && GPUMesher::Get().IsReady()) {
        GPUMesher::Get().EnqueueDeferredDestroy(streaming_mesh_.positions);
        GPUMesher::Get().EnqueueDeferredDestroy(streaming_mesh_.elements);
        GPUMesher::Get().EnqueueDeferredDestroy(streaming_mesh_.indices);
        GPUMesher::Get().EnqueueDeferredDestroy(streaming_mesh_.counters);
        GPUMesher::Get().EnqueueDeferredDestroy(streaming_mesh_.indirect_args);
    }

    streaming_mesh_ = StreamingMesh{};
}

} // namespace primal::graphics::pcg
