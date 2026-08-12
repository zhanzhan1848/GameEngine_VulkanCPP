#pragma once

// Phase 1 LOD module — recursive meshopt_simplify.
//
// Given a hi-res ProcessableMesh, produces up to `max_levels` lower-detail
// copies. Each level's index count is approximately `ratio` of the previous
// level's. meshopt_simplify preserves vertex attributes by leaving the
// vertex pool untouched — we then compact the pool to remove unreferenced
// verts and keep positions/normals/tangents/colors/uv_sets coherent.
//
// The returned vector contains levels [1..N]. The caller's input remains
// the implicit level 0 — combine as `{hi} ++ Run(hi, ...)` for the full
// chain.
//
// Stopping rules:
//   - source index count drops below 12 (can't simplify a single triangle)
//   - simplified index count == source index count (meshopt hit target_error)
//   - reached max_levels

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::lod {

struct Params {
    // Target index_count for level N = ratio * index_count of level N-1.
    // 0.5 = each level is half the previous. Must be in (0, 1).
    f32     ratio{0.5f};

    // meshopt_simplify max normalized deviation error. 0.01 ≈ 1% — typical
    // for visible-quality LODs. Higher = more aggressive simplification.
    f32     target_error{0.01f};

    // Stop after this many levels. 0 = no-op (empty output).
    u32     max_levels{6};

    // If true, meshopt_simplify gets the "don't move border verts" flag.
    // Slower convergence but prevents edge seams in tile-style meshes.
    bool    lock_borders{false};

    // meshopt_optimizeVertexCache after each simplify step. Near-free, so
    // default on.
    bool    optimize_vcache{true};
};

// Returns LOD levels [1..N]. Each entry's positions/indices/normals/etc
// form a self-contained mesh (vertex pool compacted). Empty input or
// invalid ratio → empty output + warning.
utl::vector<ProcessableMesh> Run(const ProcessableMesh& hi,
                                 const Params& params,
                                 utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::lod
