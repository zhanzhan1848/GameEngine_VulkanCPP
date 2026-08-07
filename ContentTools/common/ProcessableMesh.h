#pragma once

// Intermediate representation (IR) for the Phase 1 AI asset pipeline.
// M2 populates UVSetPurpose / UVSet / ProcessableMesh / ProcessableLod / ProcessableScene.
// Design: see ~/.claude/plans/memoized-dancing-boot.md and
// Docs/2026-08-07-asset-pipeline-phase1-library-survey.md.

namespace primal::tools {

// TODO(M2): bring in primal engine types via ToolsCommon.h
// TODO(M2): UVSetPurpose enum (Texture / Lightmap / Detail / Custom).
// TODO(M2): UVSet { UVSetPurpose purpose; utl::vector<math::v2> coords; }.
// TODO(M2): ProcessableMesh { positions / indices / normals / tangents / uv_sets / ... }.
// TODO(M2): ProcessableLod / ProcessableScene.

}  // namespace primal::tools
