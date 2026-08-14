#pragma once

// SceneBlobReader: parses scene_data.buffer back into PackedMesh views.
// Reverse of SceneBlobWriter::Serialize (and Geometry.cpp pack_mesh_data).
//
// Used by:
//   - ProcessAIAsset (M9) to re-parse ImportFbx/ImportObjAPI output before
//     re-running pipeline modules.
//   - ContentToolsCLI --validate (M9) to walk a .model file's meshlets.
//
// Zero-copy: PackedMeshView holds pointers into the source buffer. Caller
// must keep the buffer alive for the view's lifetime.

#include "ToolsCommon.h"
#include "../Engine/Utilities/IOStream.h"
#include "Geometry.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"

namespace primal::tools::pipeline {

// Read-only view over one mesh's packed serialization. All byte ranges
// (position_buffer, element_buffer, indices, meshlet_*) are unowned.
struct PackedMeshView {
    std::string                 name;
    u32                         lod_id{u32_invalid_id};
    u32                         material_idx{u32_invalid_id};
    u32                         elements_size{0};
    elements::elements_type::type elements_type{elements::elements_type::position_only};
    u32                         num_vertices{0};
    u32                         index_size{0};
    u32                         num_indices{0};
    f32                         lod_threshold{-1.f};

    const u8*                   position_buffer{nullptr};   // 12 B × num_vertices
    const u8*                   element_buffer{nullptr};    // elements_size × num_vertices
    const u8*                   index_buffer{nullptr};      // index_size × num_indices

    u32                         meshlet_count{0};
    const mesh::meshlet*        meshlets{nullptr};
    u32                         meshlet_vertex_count{0};
    const u32*                  meshlet_vertices{nullptr};
    u32                         meshlet_triangle_count{0};
    const u8*                   meshlet_triangles{nullptr};

    // SDF data is shallow-copied; reader does NOT walk into sdf.data/voxels/
    // vector_field arrays because Phase 1 validation doesn't need them.
    // Future Phase 2 readers can extend PackedMeshView with SDF pointers.
};

// Read one PackedMesh from the blob. On success, `cursor` advances past
// the consumed bytes and the function returns true. On error (truncated
// blob, missing magic, unknown elements_type), returns false and leaves
// cursor unchanged.
bool ReadNextPackedMesh(const u8*& cursor, const u8* end, PackedMeshView& out,
                        utl::vector<ErrorReport>& errors);

// Walk all meshes in a scene_data.buffer. Calls visitor(mesh_index, view)
// for each successfully-parsed mesh. Returns count visited. Stops on first
// parse error.
u32 WalkSceneBlob(const scene_data& data,
                  const std::function<void(u32, const PackedMeshView&)>& visitor,
                  utl::vector<ErrorReport>& errors);

// Phase 2 (M12.2): parsed scene wrapper contents. Used by PackedMeshDecoder
// to rebuild ProcessableScene.name + .materials before decoding meshes.
// `lod_group_count` is informational — Phase 2 PackedMeshDecoder flattens
// all (lod_group, mesh) pairs into ProcessableScene.lods[0].meshes, but
// future multi-lod_group consumers can use this to drive their own grouping.
struct SceneHeader {
    std::string                 scene_name;
    utl::vector<material>       materials;
    u32                         lod_group_count{0};
};

// Parse the scene wrapper (scene_name + materials + lod_group_count) without
// touching per-lod_group or per-mesh data. On success, `cursor` advances
// past the wrapper's u32 lod_group_count, landing at the first lod_group's
// name_size u32 — the caller then iterates skip_lod_group_header +
// ReadNextPackedMesh to walk the bodies.
//
// Returns false on truncation or unreasonable counts; `errors` carries
// diagnostic detail. Cursor is left unchanged on failure.
bool ReadSceneHeader(const u8*& cursor, const u8* end, SceneHeader& out,
                     utl::vector<ErrorReport>& errors);

// Convenience overload: ReadSceneHeader on a scene_data.buffer. Cursor
// starts at byte 0 of `data.buffer`.
bool ReadSceneHeader(const scene_data& data, SceneHeader& out,
                     utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::pipeline
