#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/Nodes/ScatterContext.h"
#include "Geometry/Geometry.h"
#include <cstring>

namespace primal::graphics::pcg {

// Precisely scatters points along geometry curves using arc-length parameterization.
// For each point, computes position via compute_point_at(t) and rotation via
// compute_tangent_at(t), producing curve-aligned instances.
//
// Pin layout:
//   Outputs: [0] PointSet — points on curves with curve-aligned RotationY
//   Inputs:  (none)
//
// Parameters:
//   target_count    — total number of points to distribute across all curves
//   position_jitter — perpendicular offset from curve (0 = exactly on curve)
//   seed            — random seed for jitter
//
// Point distribution: each curve gets points proportional to its total_length.
// RotationY convention: atan2(-tangent.z, tangent.x) — CCW around +Y, forward=+X at angle=0.
class ScatterOnGeometryNode : public PCGNode {
public:
    std::vector<primal::geometry::GeometryHandle> geometry_handles;
    u32 target_count{100};
    f32 position_jitter{0.0f};
    u32 seed{0};

    ScatterOnGeometryNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "ScatterOnGeometry"; }

    void Execute() override {
        if (geometry_handles.empty() || target_count == 0) return;

        // Compute total length and per-curve allocation
        f32 total_length = 0.0f;
        std::vector<f32> lengths(geometry_handles.size());
        for (size_t i = 0; i < geometry_handles.size(); ++i) {
            lengths[i] = primal::geometry::compute_total_length(geometry_handles[i]);
            total_length += lengths[i];
        }
        if (total_length < 1e-6f) return;

        // Distribute points proportional to length
        std::srand(seed);
        u32 allocated = 0;
        auto* out = CreateOutput<PCGPointSet>(0);
        std::vector<math::v3> positions;
        std::vector<f32> rotationYs;

        for (size_t ci = 0; ci < geometry_handles.size(); ++ci) {
            u32 count = static_cast<u32>(target_count * (lengths[ci] / total_length) + 0.5f);
            if (ci == geometry_handles.size() - 1) count = target_count - allocated;
            if (count == 0) continue;
            allocated += count;

            for (u32 i = 0; i < count; ++i) {
                f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(count);
                t = std::clamp(t, 0.0f, 1.0f);

                math::v3 pos = primal::geometry::compute_point_at(geometry_handles[ci], t);
                math::v3 tangent = primal::geometry::compute_tangent_at(geometry_handles[ci], t);

                f32 angle = std::atan2(-tangent.z, tangent.x);

                // Apply jitter perpendicular to tangent in XZ plane
                if (position_jitter > 0.0f) {
                    // Normal in XZ plane (perpendicular to tangent)
                    math::v3 normal{-tangent.z, 0.0f, tangent.x};
                    f32 nlen = std::sqrt(normal.x * normal.x + normal.z * normal.z);
                    if (nlen > 1e-6f) {
                        normal.x /= nlen;
                        normal.z /= nlen;
                    }
                    f32 jitter = ((std::rand() / f32(RAND_MAX)) - 0.5f) * 2.0f * position_jitter;
                    pos.x += normal.x * jitter;
                    pos.z += normal.z * jitter;
                }

                positions.push_back(pos);
                rotationYs.push_back(angle);
            }
        }

        out->Init(static_cast<u32>(positions.size()));
        for (u32 i = 0; i < out->count; ++i) {
            out->positions[i] = positions[i];
            out->SetAttr(i, PCGAttr::Density, 1.0f);
            out->SetAttr(i, PCGAttr::ScaleX, 1.0f);
            out->SetAttr(i, PCGAttr::ScaleY, 1.0f);
            out->SetAttr(i, PCGAttr::ScaleZ, 1.0f);
            out->SetAttr(i, PCGAttr::RotationY, rotationYs[i]);
            out->SetAttr(i, PCGAttr::MeshIndex, 0.0f);
            out->SetAttr(i, PCGAttr::Seed, static_cast<f32>(std::rand()));
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
        if (std::strcmp(name, "target_count") == 0)    { target_count = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "position_jitter") == 0)  { position_jitter = value; return true; }
        if (std::strcmp(name, "seed") == 0)             { seed = static_cast<u32>(value); return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 3;
    static constexpr u32 kPinCount = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor ScatterOnGeometryNode::kParams[] = {
    {"target_count",    "Scatter", PCGParamType::UInt,  {1,10000,1},           PCG_OFFSETOF(ScatterOnGeometryNode, target_count),    sizeof(target_count),    nullptr},
    {"position_jitter", "Scatter", PCGParamType::Float, {0.0f,5.0f,0.01f},    PCG_OFFSETOF(ScatterOnGeometryNode, position_jitter), sizeof(position_jitter), nullptr},
    {"seed",            "Scatter", PCGParamType::UInt,  {0,9999,1},            PCG_OFFSETOF(ScatterOnGeometryNode, seed),             sizeof(seed),            nullptr},
};
inline const PCGPinDescriptor ScatterOnGeometryNode::kPins[] = {
    {"curve_points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
