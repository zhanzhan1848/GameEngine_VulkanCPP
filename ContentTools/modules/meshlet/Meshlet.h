#pragma once

// Phase 1 meshlet module — meshopt_buildMeshlets with the runtime's
// 256/128 cap (Engine/Graphics/Nanite/MeshletSynthesis.h:15-16).
//
// Note: this differs from Geometry.cpp:287-350 which uses 64/124. The
// legacy code path is preserved for ImportFbx output; the new pipeline
// uses the runtime's actual limit. Per the plan, Stage7's GPU indirect
// constraint means triangle_count <= 128 is the safe upper bound.
//
// Pipeline: optimizeVertexCache → buildMeshlets → computeMeshletBounds.
// Output (MeshletData) is ready for SceneBlobWriter serialization.

#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"

namespace primal::tools::meshlet {

struct Params {
    // Hard upper bounds. meshopt_buildMeshlets may produce meshlets with
    // fewer verts/tris but never more.
    u32     max_vertices{256};
    u32     max_triangles{128};

    // 0 = no cone culling optimization, 1 = aggressive. 0.5 is the
    // meshopt default and a good balance.
    f32     cone_weight{0.5f};

    // meshopt_optimizeVertexCache before buildMeshlets. Cheap and improves
    // intra-meshlet vertex reuse; default on.
    bool    optimize_vcache{true};

    // Fill mesh::meshlet.center/radius/cone_apex/cone_axis/cone_cutoff via
    // meshopt_computeMeshletBounds. Needed for GPU culling.
    bool    compute_bounds{true};
};

// Build meshlets from `m`. Empty input → empty output + warning.
// On success, out.meshlets.size() > 0 and every meshlet's vertex_count ≤
// max_vertices and triangle_count ≤ max_triangles.
bool Run(const ProcessableMesh& m, const Params& params,
         MeshletData& out,
         utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::meshlet
