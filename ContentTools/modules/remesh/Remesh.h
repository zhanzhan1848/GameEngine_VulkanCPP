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
    // Default Isotropic: faster than Adaptive (no curvature computation).
    // The avg-edge AUTO target + large-mesh guard (skip >5000 tris) makes
    // this practical for multi-scale scenes. Adaptive is available for
    // meshes where curvature-aware density control is needed.
    Mode    mode             {Mode::Isotropic};
    // Isotropic / Adaptive: target edge length in model units.
    // Decimate: ignored (use ratio instead).
    // ** SPECIAL VALUE 0.0f = AUTO from bbox **: when target_edge_length <= 0,
    // the actual target is computed per-mesh as bbox_diagonal / auto_subdiv.
    // This is the correct mode for multi-mesh scenes where submeshes have
    // wildly different scales (e.g. Sponza: 5m pillars vs 0.5m vases).
    // A fixed target_edge_length that's fine for one mesh will collapse or
    // over-tessellate another.
    f32     target_edge_length {0.0f};

    // AUTO mode only: target = avg_edge_length / auto_subdiv.
    //   subdiv=1 → keep existing density (Adaptive mode min/max range prevents
    //              over-collapse of small triangles while splitting large ones)
    //   subdiv=2 → double density (halve edge lengths, 4× triangles, slow SDF)
    // Default 1: with Adaptive mode, the min=target*0.5 / max=target*2.0 range
    // preserves edges shorter than min (protecting fine detail) while splitting
    // edges longer than max (breaking up large flat triangles). This is the
    // best balance for multi-scale scenes like Sponza.
    u32     auto_subdiv      {1};

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
