#pragma once

// Bridge between legacy primal::tools::mesh (Geometry.h:118) and the Phase 1
// ProcessableMesh IR (ProcessableMesh.h).
//
// The legacy `mesh` struct has a dual role — it's both the importer-filled IR
// AND the packed-output container (positions/normals/uv_sets feed
// pack_mesh_data, while vertices/position_buffer/element_buffer are filled in
// by packing). Phase 1 splits these concerns:
//   - Importers (FbxImporter / ObjImporter) keep producing `mesh` as before.
//   - to_processable() lifts the SoA IR fields into ProcessableMesh for the
//     new pipeline modules to operate on.
//   - SceneBlobWriter (M8) consumes ProcessableMesh and emits scene_data.buffer.
//
// to_processable reads raw_indices (NOT indices). `indices` is the post-pack
// index buffer filled by Geometry.cpp's pack_mesh_data; raw_indices is the
// importer-produced triangle list, which is what ProcessableMesh.indices
// represents.

#include "ToolsCommon.h"
#include "Geometry.h"           // primal::tools::mesh
#include "ProcessableMesh.h"    // primal::tools::ProcessableMesh

namespace primal::tools {

// Lifts legacy `mesh` SoA IR fields into ProcessableMesh. Copies positions,
// raw_indices (→ indices), normals, tangents, colors, material_idx, name, and
// every entry of mesh.uv_sets into ProcessableMesh.uv_sets with default
// UVSetPurpose::Texture. Output-side fields of `mesh` (vertices /
// position_buffer / element_buffer / meshlets / sdf / lod_threshold / lod_id)
// are NOT carried over — they're packing artifacts, not IR.
void to_processable(const mesh& src, ProcessableMesh& dst);

// Inverse of to_processable: writes ProcessableMesh IR fields back into the
// legacy `mesh` SoA layout. Populates positions / raw_indices / normals /
// tangents / colors / uv_sets / material_idx / name. Clears any
// packing-artifact fields on dst (vertices, indices, position_buffer,
// element_buffer, meshlets, sdf) so a fresh pack_mesh_data run starts clean.
void from_processable(const ProcessableMesh& src, mesh& dst);

}  // namespace primal::tools
