#pragma once

// Bidirectional converters between primal::tools::mesh (legacy SoA buffer,
// see Geometry.h) and primal::tools::ProcessableMesh (new IR).
// M2 implements to_processable / from_processable.
//
// Constraint: only the SoA portion of `mesh` is converted. The dual-role trap
// (mesh both as runtime SoA and IR) is avoided by always routing through
// ProcessableMesh for Phase 1 pipeline operations.

namespace primal::tools {

// TODO(M2): ProcessableMesh to_processable(const mesh& legacy);
// TODO(M2): mesh from_processable(const ProcessableMesh& ir);

}  // namespace primal::tools
