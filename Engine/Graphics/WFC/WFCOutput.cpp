// Engine/Graphics/WFC/WFCOutput.cpp
//
// Task 10 (Phase A.3): WFCOutput implementation.
//
// Drain pattern: ConsumeSteps first drains the entire buffer into a local
// vector, then counts Collapse records precisely, allocates the PCGPointSet,
// and writes points in a second pass. This avoids the plan's broken
// "count-then-write" approach (which would re-Consume an empty buffer in the
// second pass) while keeping exact sizing.
#include "WFCOutput.h"

#include "WFCStepBuffer.h"
#include "WFCTileRegistry.h"
#include "WFCTypes.h"

#include <vector>

namespace primal::graphics::wfc {

using primal::graphics::pcg::PCGAttr;
using primal::graphics::pcg::PCGPointSet;

PCGPointSet WFCOutput::ConsumeSteps(WFCStepBuffer& buf,
                                    const WFCTileRegistry& registry,
                                    f32 cell_size) {
    // Pass 1: drain the entire buffer into a local vector. Consume is
    // destructive, so we must snapshot before sizing the output.
    std::vector<WFCStep> drained;
    {
        constexpr u32 kBatch = 256;
        WFCStep batch[kBatch];
        while (true) {
            u32 n = buf.Consume(batch, kBatch);
            if (n == 0) break;
            for (u32 i = 0; i < n; ++i) {
                drained.push_back(batch[i]);
            }
            if (n < kBatch) break;
        }
    }

    // Pass 2: count Collapse records so Init can size positions/attrs exactly.
    u32 collapse_count = 0;
    for (const WFCStep& s : drained) {
        if (s.kind == WFCStepKind::Collapse) {
            ++collapse_count;
        }
    }

    PCGPointSet result;
    result.Init(collapse_count, static_cast<u32>(PCGAttr::Count));

    // Pass 3: write one point per Collapse record.
    u32 out_idx = 0;
    for (const WFCStep& s : drained) {
        if (s.kind != WFCStepKind::Collapse) continue;

        const WFCTile& tile = registry.Get(s.tile);
        result.positions[out_idx] = math::v3{
            static_cast<f32>(s.coord.x) * cell_size,
            static_cast<f32>(s.coord.y) * cell_size,
            static_cast<f32>(s.coord.z) * cell_size,
        };
        result.SetAttr(out_idx, PCGAttr::ScaleX, 1.0f);
        result.SetAttr(out_idx, PCGAttr::ScaleY, 1.0f);
        result.SetAttr(out_idx, PCGAttr::ScaleZ, 1.0f);
        result.SetAttr(out_idx, PCGAttr::RotationY, 0.0f);
        result.SetAttr(out_idx, PCGAttr::MeshIndex,
                       static_cast<f32>(static_cast<u32>(tile.mesh_handle)));
        result.SetAttr(out_idx, PCGAttr::TechniqueIndex, 0.0f);
        ++out_idx;
    }

    return result;
}

} // namespace primal::graphics::wfc
