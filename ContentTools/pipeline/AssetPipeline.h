#pragma once

// AssetPipeline: Phase 1 orchestration layer. M8 populates Config + Run.
//
// Module order (fixed in Phase 1, DAG in Phase 2):
//   repair -> remesh -> subdivide -> uvatlas -> lod -> meshlet
// Each module is gated by an enable flag in pipeline::Config.

namespace primal::tools::pipeline {

// TODO(M8): struct Config { enable_repair / enable_remesh / ... ; per-module params }.
// TODO(M8): struct Result { ProcessableScene scene; utl::vector<PackedMesh> packed;
//           utl::vector<collision::Hull> hulls; utl::vector<ErrorReport> warnings; }.
// TODO(M8): void Run(ProcessableScene&& in, const Config&, Result&, progression*).

}  // namespace primal::tools::pipeline
