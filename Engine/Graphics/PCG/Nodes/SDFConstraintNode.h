#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstring>

namespace primal::graphics::pcg {

// Filters a point set by signed distance field values. Keeps points whose SDF
// distance falls within [min_dist, max_dist].
//
// Pin layout:
//   Inputs:  [0] PointSet — candidate points to filter
//            [1] Field    — SDF field for distance evaluation
//   Outputs: [0] PointSet — filtered points that pass the distance test
//
// Parameters:
//   min_dist — minimum SDF distance (inclusive). Points closer than this are removed.
//   max_dist — maximum SDF distance (inclusive). Points farther than this are removed.
//
// SDF convention: positive = outside surface, negative = inside surface.
// Use min_dist > 0 to keep points outside geometry (e.g., above ground).
// Use min_dist < 0, max_dist > 0 to keep points near the surface.
//
// Example: min_dist=0.5, max_dist=100 with a ground plane SDF keeps points
// that are 0.5 to 100 units above the ground.
class SDFConstraintNode : public PCGNode {
public:
    f32 min_dist{0.1f};
    f32 max_dist{1000.0f};

    SDFConstraintNode() {
        inputs.resize(2);
        inputs[0].expected_type = PCGDataType::PointSet;
        inputs[1].expected_type = PCGDataType::Field;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "SDFConstraint"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        auto* sdf = inputs[1].AsField();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);

        std::vector<math::v3> kept;
        std::vector<u32> indices;

        for (u32 i = 0; i < src->count; ++i) {
            f32 d = sdf ? sdf->SampleFloat(src->positions[i]) : 1.0f;
            if (d >= min_dist && d <= max_dist) {
                kept.push_back(src->positions[i]);
                indices.push_back(i);
            }
        }

        out->Init(static_cast<u32>(kept.size()));
        for (u32 i = 0; i < out->count; ++i) {
            out->positions[i] = kept[i];
            for (u32 a = 0; a < src->attr_stride && a < out->attr_stride; ++a) {
                out->attrs[i * out->attr_stride + a] =
                    src->attrs[indices[i] * src->attr_stride + a];
            }
        }
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
        if (std::strcmp(name, "min_dist") == 0) { min_dist = value; return true; }
        if (std::strcmp(name, "max_dist") == 0) { max_dist = value; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 2;
    static constexpr u32 kPinCount = 3;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor SDFConstraintNode::kParams[] = {
    {"min_dist", "Constraint", PCGParamType::Float, {-1000,1000,0.1f}, PCG_OFFSETOF(SDFConstraintNode, min_dist), sizeof(min_dist), nullptr},
    {"max_dist", "Constraint", PCGParamType::Float, {-1000,1000,0.1f}, PCG_OFFSETOF(SDFConstraintNode, max_dist), sizeof(max_dist), nullptr},
};
inline const PCGPinDescriptor SDFConstraintNode::kPins[] = {
    {"points", 0, PCGDataType::PointSet, true},
    {"sdf",    1, PCGDataType::Field,    true},
    {"points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
