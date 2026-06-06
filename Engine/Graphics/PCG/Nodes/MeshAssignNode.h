#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstdlib>
#include <numeric>

namespace primal::graphics::pcg {

// Assigns a mesh_index to each point via weighted random selection.
// The mesh_index maps to a mesh slot in the renderer's mesh list.
//
// Pin layout:
//   Inputs:  [0] PointSet — input points
//   Outputs: [0] PointSet — same points with MeshIndex attribute set
//
// Parameters:
//   weights — per-mesh-slot selection weights. Normalized internally.
//             e.g., {0.7f, 0.3f} means ~70% get mesh_index=0, ~30% get mesh_index=1.
//             Single weight {1.0f} assigns all points to mesh_index=0.
//
// The mesh_index stored in PCGAttr::MeshIndex maps to the renderer's mesh array.
// At render time, each PCGInstanceData::mesh_index selects which mesh to draw.
class MeshAssignNode : public PCGNode {
public:
    std::vector<f32> weights{1.0f};

    MeshAssignNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "MeshAssign"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        out->positions = src->positions;
        out->attrs = src->attrs;

        if (weights.empty()) return;

        // Build cumulative weights
        std::vector<f32> cumulative(weights.size());
        std::partial_sum(weights.begin(), weights.end(), cumulative.begin());
        f32 total = cumulative.back();
        if (total <= 0.0f) return;

        std::srand(12345);
        for (u32 i = 0; i < out->count; ++i) {
            f32 r = (std::rand() / f32(RAND_MAX)) * total;
            u32 slot = 0;
            for (; slot < cumulative.size() - 1; ++slot) {
                if (r < cumulative[slot]) break;
            }
            out->SetAttr(i, PCGAttr::MeshIndex, static_cast<f32>(slot));
        }
    }
};

} // namespace primal::graphics::pcg
