#pragma once

// Phase 1 subdivide module. Two PMP schemes:
//   - Loop:          triangle-only, smooth surface (Charles Loop 1987)
//   - CatmullClark:  general polygon mesh, smooth surface (Catmull & Clark 1978)
//
// Operates in-place on ProcessableMesh IR. Auxiliary attributes (normals,
// tangents, colors, uv_sets) are carried through the PMP run as vertex
// properties — but the subdivision interpolates new vertex positions, so
// the carried attributes end up at the *old* vertices; the new vertices
// get default zeros. Callers should re-derive normals/tangents/uvs after
// subdivision (e.g. via a future re-derivation pass or the uvatlas module
// for uvs).
//
// One PMP call = one subdivision step. levels=N applies the scheme N times.
// For Loop on a triangle mesh, levels=N multiplies triangle count by 4^N.

#include "ToolsCommon.h"
#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::subdivide {

enum class Scheme : u8 {
    Loop         = 0,   // pmp::loop_subdivision (triangles only)
    CatmullClark = 1,   // pmp::catmull_clark_subdivision (general polygons)
};

struct Params {
    Scheme  scheme {Scheme::Loop};
    u32     levels {1};   // number of subdivision steps (each ≈ 4× triangles for Loop)
};

// Runs the selected subdivision scheme on `io` in-place. Returns true on
// success. Loop scheme throws if the input is not a pure triangle mesh;
// that exception is caught and surfaced as an Error-severity ErrorReport.
bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::subdivide
