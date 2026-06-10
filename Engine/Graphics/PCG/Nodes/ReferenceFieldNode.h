#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/PCG/PCGSDFReadbackManager.h"
#include <cstring>

namespace primal::graphics::pcg {

// Outputs a reference field backed by FieldRegistry (e.g., GlobalSDF).
// Used to query signed distance to scene geometry for constraint filtering.
//
// Pin layout:
//   Outputs: [0] Field — PCGReferenceField with sampled SDF values
//   Inputs:  (none)
//
// Parameters:
//   semantic      — which field to reference from FieldRegistry (default: GlobalSDF)
//   readback_mgr  — optional SDF readback manager for real GPU data
//   grid_origin   — world-space origin of the SDF grid (for readback sampling)
//   voxel_size    — voxel spacing of the SDF grid
//
// When readback_mgr is set and data is ready, PCGReferenceField samples from the
// real GlobalSDF via trilinear interpolation. Otherwise falls back to analytic SDF.
class ReferenceFieldNode : public PCGNode {
public:
    field::FieldSemantic semantic{field::FieldSemantic::GlobalSDF};
    PCGSDFReadbackManager* readback_mgr{nullptr};
    math::v3 grid_origin{};
    f32 voxel_size{1.0f};

    ReferenceFieldNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Field;
    }

    const char* TypeName() const override { return "ReferenceField"; }

    void Execute() override {
        auto* field = CreateOutput<PCGReferenceField>(0);
        field->semantic = semantic;
        field->readback_mgr = readback_mgr;
        field->grid_origin = grid_origin;
        field->voxel_size = voxel_size;
        field->BindRegistry();
    }

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
        if (std::strcmp(name, "voxel_size") == 0) { voxel_size = value; return true; }
        return false;
    }
    bool SetParamByName(const char* name, math::v3 value) override {
        if (std::strcmp(name, "grid_origin") == 0) { grid_origin = value; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 2;
    static constexpr u32 kPinCount = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor ReferenceFieldNode::kParams[] = {
    {"voxel_size",  "Reference", PCGParamType::Float, {0.01f,10.0f,0.01f}, PCG_OFFSETOF(ReferenceFieldNode, voxel_size),  sizeof(voxel_size),  nullptr},
    {"grid_origin", "Reference", PCGParamType::Vec3,  {-1000,1000,0.1f},   PCG_OFFSETOF(ReferenceFieldNode, grid_origin), sizeof(grid_origin), nullptr},
};
inline const PCGPinDescriptor ReferenceFieldNode::kPins[] = {
    {"reference_field", 0, PCGDataType::Field, false},
};

} // namespace primal::graphics::pcg
