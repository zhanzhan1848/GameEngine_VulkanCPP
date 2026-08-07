#pragma once

// AssetPipeline: Phase 1 orchestration layer.
//
// Module order (fixed in Phase 1, DAG in Phase 2):
//   repair -> remesh -> subdivide -> uvatlas -> lod -> meshlet -> (sdf)
//   -> SceneBlobWriter.
// Each module is gated by an enable flag in pipeline::Config. The default
// config matches Phase 1's primary use case: lod + meshlet on, others off.
//
// collision (VHACD) runs in parallel with the blob-writing path — its
// output stays in Result.hulls and never enters scene_data.buffer (Phase 1
// has no engine-side consumer).

#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "modules/repair/Repair.h"
#include "modules/remesh/Remesh.h"
#include "modules/subdivide/Subdivide.h"
#include "modules/uvatlas/UvAtlas.h"
#include "modules/lod/Lod.h"
#include "modules/meshlet/Meshlet.h"
#include "modules/collision/Collision.h"

namespace primal::tools::pipeline {

// Per-mesh enable flags + per-module params. Defaults match the Phase 1
// "AssetPipeline default": only lod + meshlet on. Repair/remesh/subdivide/
// uvatlas are off because they're geometry-quality knobs the caller must
// opt into per asset.
struct Config {
    bool                    enable_repair{false};
    bool                    enable_remesh{false};
    bool                    enable_subdivide{false};
    bool                    enable_uvatlas{false};
    bool                    enable_lod{true};
    bool                    enable_meshlet{true};
    bool                    enable_collision{false};
    bool                    enable_sdf{false};  // Phase 1: off (legacy SDF consumer is ImportFbx only)

    repair::Params          repair_params;
    remesh::Params          remesh_params;
    subdivide::Params       subdivide_params;
    uvatlas::Params         uvatlas_params;
    lod::Params             lod_params;
    meshlet::Params         meshlet_params;
    collision::Params       collision_params;
};

// All outputs from a single Run. `packed` parallels `scene.lods[*].meshes[*]`
// 1:1 in flattened form (lod_index encoded into PackedMesh.lod_id).
struct Result {
    ProcessableScene               scene;       // post-processing IR (mutated in-place)
    utl::vector<PackedMesh>        packed;      // per-mesh serialized form
    utl::vector<collision::Hull>   hulls;       // standalone, NOT in scene_data.buffer
    utl::vector<ErrorReport>       warnings;    // non-fatal (severity == Warning or Info)
    utl::vector<ErrorReport>       errors;      // fatal    (severity == Error)
};

// Run the pipeline. Mutates `in` in-place (modules are in-place by design).
// On any Error-severity report, downstream modules for that mesh are skipped
// but Run itself does not throw — caller decides per-error policy.
//
// SDF generation (enable_sdf) reuses the legacy Geometry.cpp code path
// via SDF.h's generate_sdf. It requires building a temporary mesh struct
// from ProcessableMesh; the cost is O(num_vertices) per mesh.
void Run(ProcessableScene&& in, const Config& cfg, Result& out);

}  // namespace primal::tools::pipeline
