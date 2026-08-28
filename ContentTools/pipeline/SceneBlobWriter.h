#pragma once

// SceneBlobWriter: serializes ProcessableScene-derived data into the
// scene_data.buffer binary layout that 4 downstream consumers depend on
// (ContentToEngine.cpp / pack_geometry.py / MeshletValidator.py / C# PrimalEditor).
//
// Contract source: Geometry.cpp pack_mesh_data :661-745 + pack_vertices :310-405.
// Magics: MSHL=0x4C48534D (ContentToEngine.cpp:185), SDF=0x20464453 (:234).
//
// Phase 1 supports static_* element types only — ProcessableMesh has no
// skeletal fields. skeletal_* types are reachable only via the legacy
// ImportFbx path that still uses the mesh struct directly.
//
// t_sign stores tangent handedness in bit 0, normal.z sign in bit 1, and
// tangent.z sign in bit 2. This changes vertex values but not the binary layout.

#include "ToolsCommon.h"
#include "../Engine/Utilities/IOStream.h"  // utl::blob_stream_writer
#include "Geometry.h"  // mesh::meshlet / mesh::sdf_data / elements::*
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"

namespace primal::tools::pipeline {

// Pick the elements_type matching what ProcessableMesh actually carries.
// Preference order: static_normal_texture > static_normal > static_color > position_only.
// Phase 1 ProcessableMesh never has skeletal data, so skeletal_* are never returned.
elements::elements_type::type determine_elements_type(const ProcessableMesh& m);

// Build a PackedMesh from pipeline IR + module outputs. Renders position_buffer
// (12 B / vert) and element_buffer (per elements_type) directly from SoA IR,
// avoiding the legacy mesh.vertices AoS intermediate. UV source is the first
// UVSet with purpose=Texture (or nullptr → all-zero UVs).
//
// lod_id / lod_threshold come from the LOD chain context (LOD 0 = u32_invalid_id
// sentinel kept consistent with the legacy path).
PackedMesh BuildPackedMesh(const ProcessableMesh& m,
                           const MeshletData& meshlets,
                           u32 lod_id, f32 lod_threshold,
                           utl::vector<ErrorReport>& errors);

// Serialize a PackedMesh to `blob` using the byte-exact pack_mesh_data layout.
// Caller must size blob ahead of time using GetPackedMeshSize.
void Serialize(const PackedMesh& pm, utl::blob_stream_writer& blob);

// Compute the byte size a PackedMesh will occupy once serialized. Use this
// to size the backing buffer before calling Serialize.
size_t GetPackedMeshSize(const PackedMesh& pm);

// ---- Phase 2 (M12.3): full-scene wrapper serialization -------------------
//
// SerializeScene emits the full scene_data.buffer layout: scene_name +
// materials + 1 lod_group containing all `meshes`. Byte-for-byte alignment
// with Geometry.cpp::pack_data (lines 865-910). Use this when re-encoding a
// pipeline::Result for ProcessAIAsset output.
//
// The lod_group is emitted with an empty name — ProcessableScene has no
// lod_group_name concept, and the legacy pack_data path typically emits a
// single nameless group anyway.
//
// Memory ownership: caller allocates a buffer of GetSceneSize bytes and
// wraps it in a blob_stream_writer; SerializeScene fills it. The buffer's
// allocator MUST match what downstream consumers expect (CoTaskMemAlloc on
// Windows, malloc elsewhere) — caller's responsibility, not the writer's.
size_t GetSceneSize(const std::string& scene_name,
                    const utl::vector<material>& materials,
                    const utl::vector<PackedMesh>& meshes);

void SerializeScene(const std::string& scene_name,
                    const utl::vector<material>& materials,
                    const utl::vector<PackedMesh>& meshes,
                    utl::blob_stream_writer& blob);

}  // namespace primal::tools::pipeline
