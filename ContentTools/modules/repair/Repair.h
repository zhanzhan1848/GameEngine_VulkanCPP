#pragma once

// Phase 1 repair module. Wraps PMP Library's mesh cleanup operations on the
// ProcessableMesh IR. Operates in-place: reads io, modifies io, reports any
// skipped operations or warnings into the errors vector.
//
// Bridge strategy: ProcessableMesh.positions/indices ↔ pmp::SurfaceMesh.
// Auxiliary attributes (normals / tangents / colors / uv_sets) are carried
// across the PMP run as vertex properties. New vertices introduced by
// fill_hole get default-initialized values for those properties (zero
// normals, identity UV) — downstream modules that care about per-vertex
// attributes should re-derive them after repair.
//
// Operations are bitfield-composable (Op::StitchBorders | Op::FillHoles).
// All enabled ops run in a fixed order: degenerate cleanup → stitch borders
// → fill holes → orient outward. Ordering matters: stitching must happen
// before hole-filling so the boundary loop is consistent.

#include "ToolsCommon.h"
#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

#include <vector>

// Forward-declare pmp::SurfaceMesh so the public Repair API can reference it
// without dragging the PMP (and transitively Eigen) headers into every TU.
// Repair.cpp is the single TU that actually #includes pmp/surface_mesh.h.
namespace pmp { class SurfaceMesh; }

namespace primal::tools::repair {

// Bitfield-composable operation selector.
enum class Op : u8 {
    None                  = 0,
    RemoveDegenerateFaces = 1 << 0,   // delete faces with zero area
    StitchBorders         = 1 << 1,   // merge duplicate boundary verts within stitch_distance
    FillHoles             = 1 << 2,   // pmp::fill_hole on every boundary halfedge
    OrientOutward         = 1 << 3,   // ensure consistent face winding via flood-fill
    All                   = 0x0F,
};

inline Op  operator|  (Op a, Op b) { return static_cast<Op>(static_cast<u8>(a) | static_cast<u8>(b)); }
inline Op  operator&  (Op a, Op b) { return static_cast<Op>(static_cast<u8>(a) & static_cast<u8>(b)); }
inline bool enabled(Op flags, Op test) { return (static_cast<u8>(flags) & static_cast<u8>(test)) != 0; }

struct Params {
    u8  ops              {static_cast<u8>(Op::All)};
    f32 stitch_distance  {1e-6f};   // merge boundary verts within this distance
    u32 max_hole_size    {30};      // skip holes with more boundary edges (0 = unlimited)
    bool orient_outward  {true};
};

// Runs every enabled op on `io` in-place. Returns true if any modification
// was made. Warnings (e.g. "hole exceeded max_hole_size, skipped") are pushed
// to `errors` with source_module = "repair".
bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors);

// ---- Individual operations (used internally by Run, exposed for tests) ----

// Deletes faces whose triangle area is < epsilon. Calls garbage_collection.
// Returns the number of faces removed.
u32 remove_degenerate_faces(class pmp::SurfaceMesh& m, f32 epsilon = 1e-12f);

// Compute a vertex remap that merges coincident boundary vertices within
// stitch_distance, operating directly on the ProcessableMesh IR (post-from_pmp).
// Uses IR dense vertex indices — no PMP idx mismatch. Returns merge count.
u32 compute_ir_boundary_remap(const ProcessableMesh& io, f32 stitch_distance,
                              std::vector<u32>& out_remap);

// Calls pmp::fill_hole on every boundary halfedge whose loop length is
// ≤ max_hole_size (0 = unlimited). Returns the number of holes filled.
u32 fill_holes(class pmp::SurfaceMesh& m, u32 max_hole_size);

// Flood-fill consistent face winding starting from any face. Faces whose
// shared-edge halfedges point in the same direction get their winding
// flipped. Returns the number of faces flipped.
u32 orient_outward(class pmp::SurfaceMesh& m);

}  // namespace primal::tools::repair
