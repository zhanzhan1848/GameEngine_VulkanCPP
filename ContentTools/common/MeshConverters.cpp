#include "MeshConverters.h"

namespace primal::tools {

void to_processable(const mesh& src, ProcessableMesh& dst) {
    dst.name         = src.name;
    dst.positions    = src.positions;
    dst.indices      = src.raw_indices;
    dst.normals      = src.normals;
    dst.tangents     = src.tangents;
    dst.colors       = src.colors;
    dst.material_idx = src.material_idx;

    // Legacy mesh.uv_sets carries N channels with no purpose tag. Default all
    // to Texture; uvatlas (M6) and SceneBlobWriter (M8) upgrade individual
    // sets to Lightmap/Detail when explicitly invoked.
    dst.uv_sets.clear();
    dst.uv_sets.reserve(src.uv_sets.size());
    for (const auto& src_set : src.uv_sets) {
        UVSet dst_set;
        dst_set.purpose = UVSetPurpose::Texture;
        dst_set.coords  = src_set;
        dst.uv_sets.push_back(std::move(dst_set));
    }
}

void from_processable(const ProcessableMesh& src, mesh& dst) {
    dst.name         = src.name;
    dst.positions    = src.positions;
    dst.raw_indices  = src.indices;
    dst.normals      = src.normals;
    dst.tangents     = src.tangents;
    dst.colors       = src.colors;
    dst.material_idx = src.material_idx;

    // Legacy uv_sets is a vector of coord arrays; the UVSetPurpose tag is IR-
    // only and has no representation on the legacy struct. All UVSets are
    // carried over so existing pack_mesh_data consumers (which read uv_sets[0]
    // for Texture) keep working.
    dst.uv_sets.clear();
    dst.uv_sets.reserve(src.uv_sets.size());
    for (const auto& ir_set : src.uv_sets) {
        dst.uv_sets.push_back(ir_set.coords);
    }

    // Clear packing artifacts. from_processable is invoked when a Phase 1
    // pipeline wants to re-pack through Geometry.cpp's legacy path; stale
    // artifacts from a prior pack would corrupt the next run.
    dst.vertices.clear();
    dst.indices.clear();
    dst.position_buffer.clear();
    dst.element_buffer.clear();
    dst.meshlets.clear();
    dst.meshlet_vertices.clear();
    dst.meshlet_triangles.clear();
    dst.sdf.data.clear();
    dst.sdf.voxels.clear();
    dst.sdf.vector_field.clear();
    dst.lod_threshold = -1.f;
    dst.lod_id        = u32_invalid_id;
}

}  // namespace primal::tools
