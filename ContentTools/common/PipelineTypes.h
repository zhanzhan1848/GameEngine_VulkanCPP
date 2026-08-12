#pragma once

// Derived data types produced by Phase 1 modules and consumed by
// SceneBlobWriter (M8). These are the in-flight representations between
// modules — distinct from ProcessableMesh (the input IR that modules mutate)
// and from the legacy mesh struct (which doubles as the packed-output
// container for ImportFbx).
//
// Field layout of PackedMesh mirrors pack_mesh_data (Geometry.cpp:789-873)
// field-for-field so SceneBlobWriter's serialization stays a thin adapter.

#include "ToolsCommon.h"
#include "Geometry.h"  // mesh::meshlet / mesh::sdf_data / elements::elements_type

namespace primal::tools {

// Output of the meshlet module (M7). Mirrors meshopt_buildMeshlets output
// but uses primal engine containers. Caller (SceneBlobWriter) copies these
// into mesh.meshlets / mesh.meshlet_vertices / mesh.meshlet_triangles
// before invoking pack_mesh_data.
struct MeshletData {
    utl::vector<mesh::meshlet>  meshlets;
    utl::vector<u32>            meshlet_vertices;
    utl::vector<u8>             meshlet_triangles;

    bool empty() const noexcept { return meshlets.empty(); }
};

// One submesh ready for pack_mesh_data-style serialization. Built by
// SceneBlobWriter from ProcessableMesh + per-mesh derived data (MeshletData
// from M7, sdf_data from M2's shared SDF helper).
//
// Field-by-field correspondence with pack_mesh_data (Geometry.cpp:789-873):
//   name              -> blob header
//   lod_id            -> blob header
//   material_idx      -> blob header
//   elements_type     -> determines elements_size + element_buffer layout
//   lod_threshold     -> blob header
//   position_buffer   -> 12 bytes × num_vertices
//   element_buffer    -> get_vertex_element_size(elements_type) × num_vertices
//   indices           -> packer picks u16 (num_vertices < 65536) or u32
//   meshlets          -> after MSHL magic
//   sdf               -> after SDF magic
struct PackedMesh {
    std::string                     name;
    u32                             lod_id{u32_invalid_id};
    u32                             material_idx{u32_invalid_id};
    elements::elements_type::type   elements_type{elements::elements_type::static_normal_texture};
    f32                             lod_threshold{-1.f};

    utl::vector<u8>                 position_buffer;
    utl::vector<u8>                 element_buffer;
    utl::vector<u32>                indices;

    MeshletData                     meshlets;
    mesh::sdf_data                  sdf;
};

}  // namespace primal::tools
