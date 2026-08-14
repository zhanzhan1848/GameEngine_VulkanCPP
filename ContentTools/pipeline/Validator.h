#pragma once

// Meshlet validator: walks a scene_data.buffer and checks meshlet data
// against the runtime contract (256 verts / 128 tris cap + index bounds +
// non-zero bounds + valid offsets). C++ port of MeshletValidator.py.
//
// Used by ContentToolsCLI --validate (M9). Phase 1 only checks the
// invariants the engine's GPU culling actually depends on; deeper checks
// (back-face cones, coverage) are deferred until runtime traces exist.

#include "ToolsCommon.h"
#include "Geometry.h"  // mesh::meshlet
#include "common/ErrorReport.h"

namespace primal::tools::pipeline {

// Runtime meshlet cap (Engine/Graphics/Nanite/MeshletSynthesis.h:15-16).
constexpr u32 kRuntimeMaxMeshletVertices  = 256;
constexpr u32 kRuntimeMaxMeshletTriangles = 128;

struct ValidationReport {
    u32                         mesh_count{0};
    u32                         meshlet_count{0};
    u32                         meshlets_over_vertex_cap{0};
    u32                         meshlets_over_triangle_cap{0};
    u32                         meshlets_zero_bounds{0};
    u32                         meshlets_invalid_indices{0};
    u32                         meshlets_invalid_offsets{0};
    u32                         total_errors{0};
    u32                         total_warnings{0};
};

// Validate a scene_data.buffer. Returns true if no errors (warnings allowed).
// `errors` collects per-mesh/per-meshlet diagnostic reports for caller use.
bool ValidateSceneBlob(const scene_data& data,
                       ValidationReport& report,
                       utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::pipeline
