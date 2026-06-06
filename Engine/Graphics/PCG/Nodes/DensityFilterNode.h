#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/Nodes/ScatterContext.h"

namespace primal::graphics::pcg {

// Filters points by their Density attribute value. Keeps points where
// min_density <= Density <= max_density.
//
// Pin layout:
//   Inputs:  [0] PointSet — input points with Density attribute populated
//   Outputs: [0] PointSet — filtered points
//
// Parameters:
//   min_density — minimum density value (inclusive, default 0.0)
//   max_density — maximum density value (inclusive, default 1.0)
//
// Typically placed after FieldScatterNode to remove low-density outliers,
// producing more natural-looking clusters.
class DensityFilterNode : public PCGNode {
public:
    f32 min_density{0.0f};
    f32 max_density{1.0f};

    DensityFilterNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "DensityFilter"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        out->positions = src->positions;
        out->attrs = src->attrs;

        ScatterContext::ApplyDensityFilter(*out, min_density, max_density);
    }
};

} // namespace primal::graphics::pcg
