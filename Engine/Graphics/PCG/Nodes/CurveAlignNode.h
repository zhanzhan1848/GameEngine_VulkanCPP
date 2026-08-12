#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Geometry/Geometry.h"
#include <cstring>

namespace primal::graphics::pcg {

// Aligns scattered points to geometry curve tangents by overwriting RotationY.
// Used after field-based scatter to orient instances along the curve direction.
//
// Pin layout:
//   Inputs:  [0] PointSet — scattered points (from FieldScatterNode etc.)
//   Outputs: [1] PointSet — same points with RotationY set to curve tangent angle
//
// RotationY convention: atan2(-tangent.z, tangent.x) — CCW around +Y, forward=+X at angle=0.
//
// Usage:
//   CurveAlignNode node;
//   node.geometry_handles = {curveHandle};
//   // Wire FieldScatterNode output → CurveAlignNode input
//   node.Execute();
//   // outputs[1].AsPointSet()->GetAttr(i, PCGAttr::RotationY) is curve-aligned
class CurveAlignNode : public PCGNode {
public:
    std::vector<primal::geometry::GeometryHandle> geometry_handles;

    CurveAlignNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "CurveAlign"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src || src->count == 0 || geometry_handles.empty()) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        // Copy all data
        out->positions = src->positions;
        out->attrs = src->attrs;

        // Pre-tessellate all curves for nearest-point lookup
        struct CurveData {
            primal::geometry::GeometryHandle handle;
            const std::vector<math::v3>* tess;
            std::vector<f32> cumLength; // cumulative arc length at each tess point
        };
        std::vector<CurveData> curves;
        for (auto& h : geometry_handles) {
            CurveData cd;
            cd.handle = h;
            cd.tess = &primal::geometry::tessellate(h, 0.05f);
            cd.cumLength.resize(cd.tess->size(), 0.0f);
            for (size_t j = 1; j < cd.tess->size(); ++j) {
                math::v3 diff = (*cd.tess)[j] - (*cd.tess)[j - 1];
                cd.cumLength[j] = cd.cumLength[j - 1] + std::sqrt(diff.x*diff.x + diff.y*diff.y + diff.z*diff.z);
            }
            curves.push_back(std::move(cd));
        }

        for (u32 i = 0; i < out->count; ++i) {
            const math::v3& pos = out->positions[i];

            // Find nearest point across all curves
            f32 bestDist = 1e10f;
            int bestCurve = -1;
            int bestIdx = -1;
            for (size_t ci = 0; ci < curves.size(); ++ci) {
                auto& tess = *curves[ci].tess;
                for (size_t j = 0; j < tess.size(); ++j) {
                    math::v3 diff = tess[j] - pos;
                    f32 d = diff.x*diff.x + diff.y*diff.y + diff.z*diff.z;
                    if (d < bestDist) {
                        bestDist = d;
                        bestCurve = static_cast<int>(ci);
                        bestIdx = static_cast<int>(j);
                    }
                }
            }

            if (bestCurve >= 0) {
                auto& tess = *curves[bestCurve].tess;
                // Central difference tangent
                int prev = std::max(0, bestIdx - 1);
                int next = std::min(static_cast<int>(tess.size()) - 1, bestIdx + 1);
                math::v3 tangent = tess[next] - tess[prev];
                f32 len = std::sqrt(tangent.x*tangent.x + tangent.y*tangent.y + tangent.z*tangent.z);
                if (len > 1e-6f) {
                    tangent.x /= len; tangent.y /= len; tangent.z /= len;
                } else {
                    tangent = math::v3{1, 0, 0};
                }
                f32 angle = std::atan2(-tangent.z, tangent.x);
                out->SetAttr(i, PCGAttr::RotationY, angle);
            }
        }
    }

    // --- Reflection ---
    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = 0;
        return nullptr;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount;
        return kPins;
    }

private:
    static constexpr u32 kPinCount = 2;
    static const PCGPinDescriptor kPins[];
};

inline const PCGPinDescriptor CurveAlignNode::kPins[] = {
    {"points_in",  0, PCGDataType::PointSet, true},
    {"points_out", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
