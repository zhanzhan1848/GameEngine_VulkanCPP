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
    // Matches MAX_FRAMES_IN_FLIGHT (RHI/Core/RHITypes.h:22). Hardcoded to keep
    // the header free of RHI includes; static_assert in the cpp would catch
    // drift but GlobalSDFMeshNode is header-only — if MAX_FRAMES_IN_FLIGHT ever
    // changes, update this constant.
    static constexpr u32 kSlotCount  = 3;

    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor   kPins[];

    // Triple-buffered StreamingMesh — one set of GPU buffers per in-flight
    // frame slot. Producer (Execute) writes slot `execute_count_ % kSlotCount`;
    // consumer (RenderStreamingMeshes) reads the matching slot via frame_index.
    // Without triple-buffering, the previous frame's DrawIndirect can still be
    // reading GPU buffers when this frame's SurfaceNets writes them — causing
    // a cross-frame read-write race that destabilizes the mesh after a few
    // frames (see memory: streaming-mesh-cross-frame-hazard.md).
    StreamingMesh streaming_meshes_[kSlotCount];
    id::id_type   entity_ids_[kSlotCount]{
        id::invalid_id, id::invalid_id, id::invalid_id
    };
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

    // Step 3: Slot selection — use pipeline's frame counter so producer slot
    //   aligns with consumer (RenderStreamingMeshes uses frame_index % 3, where
    //   frame_index = cbIdx = bufferIndex from renderSystem). At Execute time
    //   for frame N, GetFrameCount() returns N — same value Render will use.
    //   Do NOT use a local execute_count_: when visible toggles off/on, local
    //   counter skips frames and goes out of sync with the render side.
    const u32 slot = static_cast<u32>(pipeline->GetFrameCount() % kSlotCount);
    StreamingMesh& current_mesh = streaming_meshes_[slot];

    if (!current_mesh.IsValid()) {
        current_mesh = CreateStreamingMesh(device, res, bounds_min, bounds_max);
        if (!current_mesh.IsValid()) {
            // Allocation failed (e.g. OOM). Emit sentinel, leave struct empty
            // so the next Execute retries.
            auto* out = CreateOutput<PCGGeometryData>(0);
            out->content_id = id::invalid_id;
            return;
        }
    }

    // Step 4: First-execute entity registration (per slot).
    //   Each slot registers as its own StreamingMeshRecord so the render side
    //   can filter by slot via `frame_index % kSlotCount`.
    if (entity_ids_[slot] == id::invalid_id) {
        entity_ids_[slot] = scene->RegisterStreamingMesh(
            &current_mesh, slot, bounds_min, bounds_max);
        current_mesh.entity_id = entity_ids_[slot];
    }

    // Step 5: Dispatch SurfaceNets for current slot only.
    //   Only this slot's buffers are rewritten this frame; other slots' data
    //   stays intact for their in-flight GPU readers.
    bool ok = GPUMesher::Get().GenerateSurfaceNetsFromGlobalSDF(
        nanite::GlobalSDF::Get(), bounds_min, bounds_max, res, iso_value,
        current_mesh);
    if (!ok) {
        // Dispatch failed. The meshing pass zeroes counters internally before
        // running, so on failure the GPU buffers hold zeros, not the previous
        // frame's geometry. Rather than rely on a zero-vertex indirect draw
        // being a silent no-op, tombstone the record so DrawStreamingMeshes
        // skips it entirely via the `tombstoned` check. Next Execute will
        // re-register (entity_id is reset to invalid_id below).
        scene->UnregisterStreamingMesh(entity_ids_[slot]);
        entity_ids_[slot] = id::invalid_id;
        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = id::invalid_id;
        return;
    }

    // Step 6: Bump generation for this slot and notify the draw side.
    ++current_mesh.generation;
    // Sync the struct's cached bounds with the current param values so that
    // DrawStreamingMeshes (and any culling pass that reads sm.mesh->bounds_*)
    // sees the user's latest bounds_min/bounds_max, not the values captured
    // at first-execute CreateStreamingMesh time.
    current_mesh.bounds_min = bounds_min;
    current_mesh.bounds_max = bounds_max;

    // DIAGNOSTIC disabled — per-frame MapBuffer/UnmapBuffer on positions/
    // indices/args/counters was destabilizing the deferred-release pool
    // after ~55 frames, contributing to cascade texture corruption.

    scene->UpdateStreamingMesh(
        entity_ids_[slot], current_mesh.generation,
        bounds_min, bounds_max);

    // Step 7: Emit sentinel. This node produces a GPU-resident mesh, not a
    //   content_id. Downstream TransformGeometryNode cannot consume it.
    auto* out = CreateOutput<PCGGeometryData>(0);
    out->content_id = id::invalid_id;
}

// ---------------------------------------------------------------------------
// Destructor — unregister + queue deferred destroy (all slots)
// ---------------------------------------------------------------------------
inline GlobalSDFMeshNode::~GlobalSDFMeshNode() {
    // If the node was registered, tombstone all slot records. Each record is
    // not erased immediately because the GPU may still be iterating the list
    // this frame; RenderScene::ClearTombstonedStreamingMeshes() (called at
    // frame boundary after GPU work) handles final cleanup.
    if (auto* pipeline = static_cast<StandardRenderPipeline*>(RenderPipeline::Get())) {
        if (auto* scene = pipeline->GetCurrentScene()) {
            for (u32 i = 0; i < kSlotCount; ++i) {
                if (entity_ids_[i] != id::invalid_id) {
                    scene->UnregisterStreamingMesh(entity_ids_[i]);
                }
            }
        }
    }

    // Queue each slot's 5 buffer handles for deferred destruction. On Metal
    // this is safe even while command buffers are in-flight because Metal
    // retains resources referenced by encoders. A Vulkan/D3D12 backend would
    // need a GPU fence wait first.
    if (GPUMesher::Get().IsReady()) {
        for (u32 i = 0; i < kSlotCount; ++i) {
            if (streaming_meshes_[i].IsValid()) {
                GPUMesher::Get().EnqueueDeferredDestroy(streaming_meshes_[i].positions);
                GPUMesher::Get().EnqueueDeferredDestroy(streaming_meshes_[i].elements);
                GPUMesher::Get().EnqueueDeferredDestroy(streaming_meshes_[i].indices);
                GPUMesher::Get().EnqueueDeferredDestroy(streaming_meshes_[i].counters);
                GPUMesher::Get().EnqueueDeferredDestroy(streaming_meshes_[i].indirect_args);
                streaming_meshes_[i] = StreamingMesh{};
            }
        }
    }
}

} // namespace primal::graphics::pcg
