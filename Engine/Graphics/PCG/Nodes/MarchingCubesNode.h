#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/PCGTypes.h"
#include "Graphics/PCG/PCGReflection.h"
#include "Graphics/PCG/MarchingCubes.h"
#include "Content/ContentToEngine.h"
#include "Content/ProceduralMesh.h"
#include <cstring>

namespace primal::graphics::pcg {

// Generates a mesh asset from a scalar field via SurfaceNets.
//
// Closes the field → geometry loop: any PCGField (NoiseField, RasterizedField,
// ReferenceField via SDF readback) can be converted to a mesh asset at runtime,
// which can then be registered for rendering via PipelineRegisterMeshEntity.
//
// Pin layout:
//   Inputs:  [0] Field  — PCGField to sample (any implementation)
//   Outputs: [0] Geometry — PCGGeometryData carrying the generated mesh content_id
//
// Parameters:
//   bounds_min/max — world-space AABB of the sampling volume
//   resolution     — per-axis voxel count (4..128). Total cells = resolution^3.
//                    Default 64; 96 is the comfort ceiling for interactive use.
//   iso_value      — threshold where the surface lives. For SDF use 0.0;
//                    for noise/density fields tune to taste (e.g. 0.5).
//   algorithm      — reserved for future (0=SurfaceNets, 1=ClassicMC)
//
// Lifecycle:
//   The node owns the generated mesh asset across re-executions. Execute()
//   destroys the previous asset (if any) before generating a new one. The
//   destructor also destroys any outstanding asset so graph teardown is leak-free.
class MarchingCubesNode : public PCGNode {
public:
    math::v3 bounds_min{-10.0f, -10.0f, -10.0f};
    math::v3 bounds_max{ 10.0f,  10.0f,  10.0f};
    u32      resolution{64};
    f32      iso_value{0.0f};
    u32      algorithm{0};

    MarchingCubesNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::Field;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Geometry;
    }

    ~MarchingCubesNode() override {
        DestroyTrackedAsset();
    }

    const char* TypeName() const override { return "MarchingCubes"; }

    void Execute() override {
        // Destroy previous asset before generating a new one.
        DestroyTrackedAsset();

        auto* field = inputs[0].AsField();
        if (!field) return;

        // Cap resolution to keep CPU time sane (128^3 ≈ 60ms on Apple Silicon).
        u32 res = resolution;
        if (res < 2) res = 2;
        if (res > 128) res = 128;

        MarchingCubesResult mesh = GenerateSurfaceNetsCPU(
            *field, bounds_min, bounds_max, res, iso_value);

        if (mesh.positions.empty() || mesh.indices.empty()) {
            // No surface extracted — emit empty geometry with invalid_id.
            auto* out = CreateOutput<PCGGeometryData>(0);
            out->content_id = id::invalid_id;
            return;
        }

        BuildAndRegisterAsset(mesh);

        auto* out = CreateOutput<PCGGeometryData>(0);
        out->content_id = last_created_id_;
    }

    // Allow C ABI to read the most recently generated mesh content_id without
    // pin traversal. Returns id::invalid_id if Execute() has not run or produced nothing.
    id::id_type GetLastCreatedId() const { return last_created_id_; }

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
        if (std::strcmp(name, "algorithm")  == 0) { algorithm = static_cast<u32>(value); return true; }
        return false;
    }
    bool SetParamByName(const char* name, math::v3 value) override {
        if (std::strcmp(name, "bounds_min") == 0) { bounds_min = value; return true; }
        if (std::strcmp(name, "bounds_max") == 0) { bounds_max = value; return true; }
        return false;
    }

private:
    // Build RHIMeshAsset from MarchingCubesResult, register it, and update
    // last_created_id_. Shared by CPU and GPU paths. Position buffer is f32x3
    // interleaved; element buffer uses content::WriteVertex (20B static_normal_texture);
    // index buffer is u32. Destroys any previously-created asset first via the
    // caller (Execute() must call DestroyTrackedAsset() before this).
    void BuildAndRegisterAsset(const MarchingCubesResult& mesh) {
        const u32 vert_count = static_cast<u32>(mesh.positions.size() / 3);
        const u32 idx_count  = static_cast<u32>(mesh.indices.size());

        graphics::rhi::RHIMeshAsset asset;
        asset.num_vertices = vert_count;
        asset.num_indices  = idx_count;
        asset.position_buffer.resize(vert_count * 12);
        asset.element_buffer.resize(vert_count * content::PROC_ELEM_STRIDE);
        asset.index_buffer.resize(idx_count * 4);

        std::memcpy(asset.position_buffer.data(), mesh.positions.data(), vert_count * 12);
        for (u32 v = 0; v < vert_count; ++v) {
            content::WriteVertex(
                asset.position_buffer.data() + v * 12,
                asset.element_buffer.data() + v * content::PROC_ELEM_STRIDE,
                mesh.positions[v * 3 + 0],
                mesh.positions[v * 3 + 1],
                mesh.positions[v * 3 + 2],
                mesh.normals [v * 3 + 0],
                mesh.normals [v * 3 + 1],
                mesh.normals [v * 3 + 2],
                mesh.uvs     [v * 2 + 0],
                mesh.uvs     [v * 2 + 1]);
        }
        std::memcpy(asset.index_buffer.data(), mesh.indices.data(), idx_count * 4);

        last_created_id_ = content::register_mesh_asset(asset);
    }

    void DestroyTrackedAsset() {
        if (last_created_id_ != id::invalid_id) {
            content::destroy_resource(last_created_id_, content::asset_type::mesh);
            last_created_id_ = id::invalid_id;
        }
    }

    static constexpr u32 kParamCount = 5;
    static constexpr u32 kPinCount = 2;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor  kPins[];

    id::id_type last_created_id_{id::invalid_id};
};

inline const PCGParamDescriptor MarchingCubesNode::kParams[] = {
    {"bounds_min", "Volume", PCGParamType::Vec3,  {-100.0f, 100.0f, 0.1f},
     PCG_OFFSETOF(MarchingCubesNode, bounds_min), sizeof(bounds_min), nullptr},
    {"bounds_max", "Volume", PCGParamType::Vec3,  {-100.0f, 100.0f, 0.1f},
     PCG_OFFSETOF(MarchingCubesNode, bounds_max), sizeof(bounds_max), nullptr},
    {"resolution", "Volume", PCGParamType::UInt,  {4.0f, 128.0f, 4.0f},
     PCG_OFFSETOF(MarchingCubesNode, resolution), sizeof(resolution), nullptr},
    {"iso_value",  "Volume", PCGParamType::Float, {-1.0f, 1.0f, 0.01f},
     PCG_OFFSETOF(MarchingCubesNode, iso_value),  sizeof(iso_value),  nullptr},
    {"algorithm",  "Volume", PCGParamType::UInt,  {0.0f, 1.0f, 1.0f},
     PCG_OFFSETOF(MarchingCubesNode, algorithm),  sizeof(algorithm),  "SurfaceNets,ClassicMC"},
};

inline const PCGPinDescriptor MarchingCubesNode::kPins[] = {
    {"field",    0, PCGDataType::Field,    true},
    {"geometry", 0, PCGDataType::Geometry, false},
};

} // namespace primal::graphics::pcg
