#include "WFCOutput.h"
#include "WFCStepBuffer.h"
#include "WFCTileRegistry.h"
#include "WFCTypes.h"
#include "WFCSolveBudget.h"

#include <vector>

namespace primal::graphics::wfc {

namespace {

// Phase B.2: shared per-point emission. Writes one point into ps at out_idx
// based on a Collapse step. Used by both ConsumeSteps (batch) and DrainStream
// (streaming) so the rotation/scale/mesh encoding stays consistent.
void WritePointToSet(const WFCStep& s, const WFCTileRegistry& registry,
                     pcg::PCGPointSet& ps, u32 out_idx, f32 cell_size) {
    const WFCTile& tile = registry.Get(s.tile);
    ps.positions[out_idx] = math::v3{
        static_cast<f32>(s.coord.x) * cell_size,
        static_cast<f32>(s.coord.y) * cell_size,
        static_cast<f32>(s.coord.z) * cell_size,
    };
    ps.SetAttr(out_idx, pcg::PCGAttr::ScaleX, 1.0f);
    ps.SetAttr(out_idx, pcg::PCGAttr::ScaleY, 1.0f);
    ps.SetAttr(out_idx, pcg::PCGAttr::ScaleZ, 1.0f);
    f32 rot_y = 0.0f;
    if (!tile.is_rotationally_symmetric) {
        constexpr f32 kHalfPi = 1.5707963267948966f;
        rot_y = static_cast<f32>(s.variant) * kHalfPi;
    }
    ps.SetAttr(out_idx, pcg::PCGAttr::RotationY, rot_y);
    ps.SetAttr(out_idx, pcg::PCGAttr::MeshIndex,
               static_cast<f32>(static_cast<u32>(tile.mesh_handle)));
    ps.SetAttr(out_idx, pcg::PCGAttr::TechniqueIndex, 0.0f);
}

// Phase B.2: shared drain loop. Pulls everything out of the buffer into a
// local vector. Consume is destructive so the buffer is empty afterward.
std::vector<WFCStep> DrainAll(WFCStepBuffer& buf) {
    std::vector<WFCStep> drained;
    constexpr u32 kBatch = 256;
    WFCStep batch[kBatch];
    while (true) {
        u32 n = buf.Consume(batch, kBatch);
        if (n == 0) break;
        for (u32 i = 0; i < n; ++i) drained.push_back(batch[i]);
        if (n < kBatch) break;
    }
    return drained;
}

} // namespace

// ---------------------------------------------------------------------------
// Phase A.3: batch consume. Unchanged behavior, refactored to use the
// WritePointToSet helper for DRY with DrainStream.
// ---------------------------------------------------------------------------
pcg::PCGPointSet WFCOutput::ConsumeSteps(WFCStepBuffer& buf,
                                         const WFCTileRegistry& registry,
                                         f32 cell_size) {
    std::vector<WFCStep> drained = DrainAll(buf);

    u32 collapse_count = 0;
    for (const WFCStep& s : drained) {
        if (s.kind == WFCStepKind::Collapse) ++collapse_count;
    }

    pcg::PCGPointSet result;
    result.Init(collapse_count, static_cast<u32>(pcg::PCGAttr::Count));

    u32 out_idx = 0;
    for (const WFCStep& s : drained) {
        if (s.kind != WFCStepKind::Collapse) continue;
        WritePointToSet(s, registry, result, out_idx, cell_size);
        ++out_idx;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Phase B.2: streaming consume. Returns only post-last-Restart Collapse
// points. Caller destroys previously-spawned entities when restart_seen.
// ---------------------------------------------------------------------------
WFCStreamDrainResult WFCOutput::DrainStream(WFCStepBuffer& buf,
                                            const WFCTileRegistry& registry,
                                            f32 cell_size) {
    WFCStreamDrainResult result;
    std::vector<WFCStep> drained = DrainAll(buf);

    // Find the index immediately after the LAST Restart in the snapshot.
    // Only Collapse steps at index >= emit_start survive.
    u32 emit_start = 0;
    for (u32 i = 0; i < drained.size(); ++i) {
        if (drained[i].kind == WFCStepKind::Restart) {
            result.restart_seen = true;
            ++result.restart_count;
            emit_start = i + 1;
        }
    }

    u32 collapse_count = 0;
    for (u32 i = emit_start; i < drained.size(); ++i) {
        if (drained[i].kind == WFCStepKind::Collapse) ++collapse_count;
    }
    result.new_points.Init(collapse_count, static_cast<u32>(pcg::PCGAttr::Count));

    u32 out_idx = 0;
    for (u32 i = emit_start; i < drained.size(); ++i) {
        if (drained[i].kind != WFCStepKind::Collapse) continue;
        WritePointToSet(drained[i], registry, result.new_points, out_idx, cell_size);
        ++out_idx;
    }
    return result;
}

} // namespace primal::graphics::wfc
