#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/Nodes/ScatterContext.h"
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace primal::graphics::pcg {

// Generates a point set by placing candidates on a jittered grid within a bounding
// volume, then probabilistically keeping each based on a density field sample.
//
// Pin layout:
//   Inputs:  [0] Field (optional) — density field for probability weighting
//   Outputs: [0] PointSet — scattered points with Density attribute set
//
// Parameters:
//   target_count        — approximate number of grid candidates to generate.
//                         Actual count may differ due to grid rounding.
//   bounds_min/max      — world-space AABB for the scatter volume.
//                         Points are placed at y = midpoint of bounds_min.y and bounds_max.y.
//   seed                — random seed for reproducible jitter and density sampling
//   points_per_unit_area — global acceptance probability multiplier (default 1.0).
//                          Final keep probability = density * points_per_unit_area.
//
// Density interpretation: if a density field is connected, its [-1,1] output is
// remapped to [0,1]. Points in high-density regions are more likely to be kept.
// If no density field is connected, density defaults to 1.0 (uniform scatter).
//
// Example: target_count=2000, bounds=[-40,1,-40] to [40,3,40], with a noise field
// produces ~1000 naturally clustered points in a horizontal band at y=2.
class FieldScatterNode : public PCGNode {
public:
    u32 target_count{1000};
    math::v3 bounds_min{-50, 0, -50};
    math::v3 bounds_max{50, 5, 50};
    u32 seed{42};
    f32 points_per_unit_area{1.0f};

    FieldScatterNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::Field;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "FieldScatter"; }

    void Execute() override {
        auto* density_field = inputs[0].AsField();

        auto* out = CreateOutput<PCGPointSet>(0);

        std::srand(seed);

        f32 range_x = bounds_max.x - bounds_min.x;
        f32 range_z = bounds_max.z - bounds_min.z;
        u32 grid_x = static_cast<u32>(std::sqrt(static_cast<f32>(target_count) * range_x / range_z));
        u32 grid_z = static_cast<u32>(target_count / std::max(grid_x, 1u));
        if (grid_x == 0) grid_x = 1;
        if (grid_z == 0) grid_z = 1;

        f32 cell_x = range_x / grid_x;
        f32 cell_z = range_z / grid_z;
        f32 y = (bounds_min.y + bounds_max.y) * 0.5f;

        std::vector<math::v3> kept;

        for (u32 iz = 0; iz < grid_z; ++iz) {
            for (u32 ix = 0; ix < grid_x; ++ix) {
                f32 base_x = bounds_min.x + (ix + 0.5f) * cell_x;
                f32 base_z = bounds_min.z + (iz + 0.5f) * cell_z;

                f32 jx = (std::rand() / f32(RAND_MAX) - 0.5f) * cell_x * 0.9f;
                f32 jz = (std::rand() / f32(RAND_MAX) - 0.5f) * cell_z * 0.9f;

                math::v3 pos{base_x + jx, y, base_z + jz};

                f32 density = 1.0f;
                if (density_field) {
                    density = density_field->SampleFloat(pos);
                    density = 0.5f + 0.5f * density; // remap [-1,1] → [0,1]
                }

                f32 threshold = std::rand() / f32(RAND_MAX);
                if (threshold < density * points_per_unit_area) {
                    kept.push_back(pos);
                }
            }
        }

        out->Init(static_cast<u32>(kept.size()));
        for (u32 i = 0; i < out->count; ++i) {
            out->positions[i] = kept[i];
        }
        ScatterContext::WriteDefaultAttributes(*out, seed);
        ScatterContext::ApplyJitter(*out, cell_x * 0.1f, seed + 1);
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
        if (std::strcmp(name, "target_count") == 0)         { target_count = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "seed") == 0)                 { seed = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "points_per_unit_area") == 0) { points_per_unit_area = value; return true; }
        return false;
    }
    bool SetParamByName(const char* name, math::v3 value) override {
        if (std::strcmp(name, "bounds_min") == 0) { bounds_min = value; return true; }
        if (std::strcmp(name, "bounds_max") == 0) { bounds_max = value; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 5;
    static constexpr u32 kPinCount = 2;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor FieldScatterNode::kParams[] = {
    {"target_count",        "Scatter", PCGParamType::UInt,  {1,100000,1},              PCG_OFFSETOF(FieldScatterNode, target_count),        sizeof(target_count),        nullptr},
    {"seed",                "Scatter", PCGParamType::UInt,  {0,9999,1},                PCG_OFFSETOF(FieldScatterNode, seed),                 sizeof(seed),                nullptr},
    {"points_per_unit_area","Scatter", PCGParamType::Float, {0.01f,100.0f,0.01f},      PCG_OFFSETOF(FieldScatterNode, points_per_unit_area), sizeof(points_per_unit_area), nullptr},
    {"bounds_min",          "Scatter", PCGParamType::Vec3,  {-1000,1000,0.1f},         PCG_OFFSETOF(FieldScatterNode, bounds_min),           sizeof(bounds_min),          nullptr},
    {"bounds_max",          "Scatter", PCGParamType::Vec3,  {-1000,1000,0.1f},         PCG_OFFSETOF(FieldScatterNode, bounds_max),           sizeof(bounds_max),          nullptr},
};
inline const PCGPinDescriptor FieldScatterNode::kPins[] = {
    {"density_field", 0, PCGDataType::Field,    true},
    {"points",        0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
