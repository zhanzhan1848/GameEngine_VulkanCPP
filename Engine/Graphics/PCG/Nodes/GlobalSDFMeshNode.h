#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/PCGTypes.h"
#include "Graphics/PCG/PCGReflection.h"
#include "Graphics/PCG/GPU/GPUMesher.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
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
//   resolution     — per-axis voxel count (4..128, default 64)
//   iso_value      — SDF threshold where the surface lives (default 0.0)
//
// Lifecycle:
//   First Execute allocates StreamingMesh + registers entity. Subsequent
//   Executes reuse the same buffers (zero counters, re-dispatch, bump
//   generation). Destructor unregisters entity + queues deferred destroy.
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

    StreamingMesh streaming_mesh_{};
    bool          registered_{false};
    u64           generation_{0};
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

// --- STUB implementations (replaced in Task 9) ---
inline GlobalSDFMeshNode::~GlobalSDFMeshNode() {}

inline void GlobalSDFMeshNode::Execute() {
    auto* out = CreateOutput<PCGGeometryData>(0);
    out->content_id = id::invalid_id;
}

} // namespace primal::graphics::pcg
