#pragma once

// Phase 1 remesh module. Three modes:
//   - Isotropic: pmp::uniform_remeshing — equal-length edges via collapse/split/flip
//   - Adaptive:  pmp::adaptive_remeshing — min/max edge-length range, curvature-aware
//   - Decimate:  igl::qslim — quadric error metric decimation (target face count)
//
// Operates in-place on ProcessableMesh IR. Auxiliary attributes (normals,
// tangents, colors, uv_sets) are carried across the PMP run as vertex
// properties — for Decimate mode, attribute interpolation is left to
// libigl's birth-vertex index map (U, G, J, I), and currently only positions
// are preserved (other channels are dropped with a warning).

#include "ToolsCommon.h"
#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::remesh {

enum class Mode : u8 {
    Isotropic = 0,   // pmp::uniform_remeshing
    Adaptive  = 1,   // pmp::adaptive_remeshing
    Decimate  = 2,   // igl::qslim (quadric simplification)
};

struct Params {
    Mode    mode             {Mode::Isotropic};
    // Isotropic / Adaptive: target edge length in model units.
    // Decimate: ignored (use ratio instead).
    f32     target_edge_length {0.05f};

    // Adaptive only: max approximation error allowed during remeshing.
    f32     adaptive_approx_error{0.001f};

    // Decimate only: target face count = source_face_count * ratio.
    // Range (0,1]. 0.5 → half the triangles.
    f32     ratio            {0.5f};

    // Isotropic / Adaptive: remeshing iterations (PMP default is 10).
    u32     iterations       {10};

    // Isotropic / Adaptive: enable back-projection to original surface.
    bool    use_projection   {true};
};

// Runs the selected remesh operation on `io` in-place. Returns true on success.
// On Decimate failure (qslim returned false, usually non-manifold input), the
// mesh is left unmodified and an Error-severity ErrorReport is pushed.
bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::remesh
