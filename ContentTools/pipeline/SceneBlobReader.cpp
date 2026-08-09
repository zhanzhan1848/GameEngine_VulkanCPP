#include "SceneBlobReader.h"

#include <cstring>

namespace primal::tools::pipeline {
namespace {

// Bounds-checked blob reader. Every read advances the cursor and verifies
// the new cursor stays within `end`. On any overrun, returns false and
// leaves cursor unchanged.
struct cursor {
    const u8*& p;
    const u8*  e;

    bool read_u32(u32& out) {
        if (p + 4 > e) return false;
        out = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        p += 4;
        return true;
    }
    bool read_f32(f32& out) {
        if (p + 4 > e) return false;
        u32 bits = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        std::memcpy(&out, &bits, sizeof(out));
        p += 4;
        return true;
    }
    bool read_bytes(const u8*& out, size_t n) {
        if (p + n > e) return false;
        out = p;
        p += n;
        return true;
    }
    bool expect_magic(u32 expected) {
        u32 v;
        if (!read_u32(v)) return false;
        return v == expected;
    }
};

// Skip the scene wrapper that pack_data writes before the first mesh:
//   u32 scene_name_size + scene_name bytes
//   u32 material_count + per-material (u32 name_size + name, u32 diffuse_size + diffuse, u32 normal_size + normal)
//   u32 lod_group_count
// Caller then walks each lod_group via skip_lod_group_header.
// On success, sets *out_lod_group_count. Returns false on truncation.
bool skip_scene_header(cursor& c, u32* out_lod_group_count,
                       utl::vector<ErrorReport>& errors) {
    u32 scene_name_size;
    if (!c.read_u32(scene_name_size)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_scene_name",
            "blob_read: truncated at scene_name_size", "blob_read"});
        return false;
    }
    if (scene_name_size > 4096) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_scene_name_size",
            "blob_read: scene_name_size unreasonably large", "blob_read"});
        return false;
    }
    {
        const u8* skip;
        if (!c.read_bytes(skip, scene_name_size)) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_scene_name_bytes",
                "blob_read: truncated at scene_name bytes", "blob_read"});
            return false;
        }
    }

    u32 material_count;
    if (!c.read_u32(material_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_count",
            "blob_read: truncated at material_count", "blob_read"});
        return false;
    }
    if (material_count > 100000) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_material_count",
            "blob_read: material_count unreasonably large", "blob_read"});
        return false;
    }
    for (u32 i = 0; i < material_count; ++i) {
        // Each material: name + diffuse_texture + normal_texture (3 length-prefixed strings).
        for (u32 s = 0; s < 3; ++s) {
            u32 str_size;
            if (!c.read_u32(str_size)) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_string",
                    "blob_read: truncated in material string", "blob_read"});
                return false;
            }
            if (str_size > 65536) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_material_string",
                    "blob_read: material string length unreasonably large", "blob_read"});
                return false;
            }
            const u8* skip;
            if (!c.read_bytes(skip, str_size)) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_string_bytes",
                    "blob_read: truncated material string bytes", "blob_read"});
                return false;
            }
        }
    }

    if (!c.read_u32(*out_lod_group_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_lod_group_count",
            "blob_read: truncated at lod_group_count", "blob_read"});
        return false;
    }
    if (*out_lod_group_count > 100000) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_lod_group_count",
            "blob_read: lod_group_count unreasonably large", "blob_read"});
        return false;
    }
    return true;
}

// Phase 2 (M12.2) variant: parses the scene wrapper AND extracts scene_name +
// materials + lod_group_count. Mirrors skip_scene_header byte-for-byte but
// populates `out`. Used by PackedMeshDecoder::DecodeScene to rebuild
// ProcessableScene.{name, materials} before iterating meshes.
//
// On success the cursor sits exactly where skip_scene_header would leave it:
// right past the lod_group_count u32, at the first lod_group's name_size.
bool read_scene_header(cursor& c, SceneHeader& out, utl::vector<ErrorReport>& errors) {
    out = SceneHeader{};

    u32 scene_name_size;
    if (!c.read_u32(scene_name_size)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_scene_name",
            "blob_read: truncated at scene_name_size", "blob_read"});
        return false;
    }
    if (scene_name_size > 4096) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_scene_name_size",
            "blob_read: scene_name_size unreasonably large", "blob_read"});
        return false;
    }
    {
        const u8* name_bytes;
        if (!c.read_bytes(name_bytes, scene_name_size)) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_scene_name_bytes",
                "blob_read: truncated at scene_name bytes", "blob_read"});
            return false;
        }
        out.scene_name.assign(reinterpret_cast<const char*>(name_bytes), scene_name_size);
    }

    u32 material_count;
    if (!c.read_u32(material_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_count",
            "blob_read: truncated at material_count", "blob_read"});
        return false;
    }
    if (material_count > 100000) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_material_count",
            "blob_read: material_count unreasonably large", "blob_read"});
        return false;
    }
    out.materials.reserve(material_count);
    for (u32 i = 0; i < material_count; ++i) {
        material mat{};
        const char* field_names[3] = {"name", "diffuse_texture", "normal_texture"};
        std::string* field_dst[3] = {&mat.name, &mat.diffuse_texture, &mat.normal_texture};
        for (u32 s = 0; s < 3; ++s) {
            u32 str_size;
            if (!c.read_u32(str_size)) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_string",
                    "blob_read: truncated in material " + std::string(field_names[s]), "blob_read"});
                return false;
            }
            if (str_size > 65536) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_material_string",
                    "blob_read: material " + std::string(field_names[s]) + " length unreasonably large",
                    "blob_read"});
                return false;
            }
            const u8* str_bytes;
            if (!c.read_bytes(str_bytes, str_size)) {
                errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_material_string_bytes",
                    "blob_read: truncated material " + std::string(field_names[s]) + " bytes", "blob_read"});
                return false;
            }
            field_dst[s]->assign(reinterpret_cast<const char*>(str_bytes), str_size);
        }
        out.materials.emplace_back(std::move(mat));
    }

    if (!c.read_u32(out.lod_group_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_lod_group_count",
            "blob_read: truncated at lod_group_count", "blob_read"});
        return false;
    }
    if (out.lod_group_count > 100000) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_lod_group_count",
            "blob_read: lod_group_count unreasonably large", "blob_read"});
        return false;
    }
    return true;
}

// Skip a lod_group wrapper: u32 lod_name_size + name + u32 mesh_count.
// Optionally captures the lod_name into `*out_name` when non-null.
// Returns the mesh_count (caller iterates that many meshes). On error,
// returns u32_invalid_id.
u32 skip_lod_group_header(cursor& c, std::string* out_name,
                           utl::vector<ErrorReport>& errors) {
    u32 lod_name_size;
    if (!c.read_u32(lod_name_size)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_lod_name",
            "blob_read: truncated at lod_name_size", "blob_read"});
        return u32_invalid_id;
    }
    if (lod_name_size > 4096) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_lod_name_size",
            "blob_read: lod_name_size unreasonably large", "blob_read"});
        return u32_invalid_id;
    }
    {
        const u8* name_bytes;
        if (!c.read_bytes(name_bytes, lod_name_size)) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_lod_name_bytes",
                "blob_read: truncated at lod_name bytes", "blob_read"});
            return u32_invalid_id;
        }
        if (out_name) {
            out_name->assign(reinterpret_cast<const char*>(name_bytes), lod_name_size);
        }
    }
    u32 mesh_count;
    if (!c.read_u32(mesh_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_mesh_count",
            "blob_read: truncated at mesh_count", "blob_read"});
        return u32_invalid_id;
    }
    return mesh_count;
}

}  // namespace

bool ReadNextPackedMesh(const u8*& cursor_in, const u8* end, PackedMeshView& out,
                        utl::vector<ErrorReport>& errors) {
    cursor c{cursor_in, end};

    // Header
    u32 name_size;
    if (!c.read_u32(name_size)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_header",
            "blob_read: truncated at name_size", "blob_read"});
        return false;
    }
    if (name_size > 1024) {  // sanity check
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_name_size",
            "blob_read: name_size unreasonably large", "blob_read"});
        return false;
    }
    const u8* name_bytes;
    if (!c.read_bytes(name_bytes, name_size)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_name",
            "blob_read: truncated at name bytes", "blob_read"});
        return false;
    }
    out.name.assign(reinterpret_cast<const char*>(name_bytes), name_size);

    if (!c.read_u32(out.lod_id) ||
        !c.read_u32(out.material_idx) ||
        !c.read_u32(out.elements_size) ||
        !c.read_u32((u32&)out.elements_type) ||
        !c.read_u32(out.num_vertices) ||
        !c.read_u32(out.index_size) ||
        !c.read_u32(out.num_indices) ||
        !c.read_f32(out.lod_threshold)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meta",
            "blob_read: truncated at header meta fields", "blob_read"});
        return false;
    }

    // Buffers
    if (!c.read_bytes(out.position_buffer, (size_t)12 * out.num_vertices)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_position",
            "blob_read: truncated at position_buffer", "blob_read"});
        return false;
    }
    if (!c.read_bytes(out.element_buffer, (size_t)out.elements_size * out.num_vertices)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_element",
            "blob_read: truncated at element_buffer", "blob_read"});
        return false;
    }
    if (!c.read_bytes(out.index_buffer, (size_t)out.index_size * out.num_indices)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_index",
            "blob_read: truncated at index_buffer", "blob_read"});
        return false;
    }

    // MSHL magic + meshlets
    if (!c.expect_magic(0x4C48534Du)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.missing_mshl",
            "blob_read: missing or misplaced MSHL magic", "blob_read"});
        return false;
    }
    if (!c.read_u32(out.meshlet_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlet_count",
            "blob_read: truncated at meshlet_count", "blob_read"});
        return false;
    }
    if (out.meshlet_count > 0) {
        if (!c.read_bytes((const u8*&)out.meshlets,
                          (size_t)out.meshlet_count * sizeof(mesh::meshlet))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlets",
                "blob_read: truncated at meshlet array", "blob_read"});
            return false;
        }
    }
    if (!c.read_u32(out.meshlet_vertex_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlet_vert_count",
            "blob_read: truncated at meshlet_vertex_count", "blob_read"});
        return false;
    }
    if (out.meshlet_vertex_count > 0) {
        if (!c.read_bytes((const u8*&)out.meshlet_vertices,
                          (size_t)out.meshlet_vertex_count * sizeof(u32))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlet_verts",
                "blob_read: truncated at meshlet_vertices array", "blob_read"});
            return false;
        }
    }
    if (!c.read_u32(out.meshlet_triangle_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlet_tri_count",
            "blob_read: truncated at meshlet_triangle_count", "blob_read"});
        return false;
    }
    if (out.meshlet_triangle_count > 0) {
        if (!c.read_bytes((const u8*&)out.meshlet_triangles,
                          (size_t)out.meshlet_triangle_count * sizeof(u8))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_meshlet_tris",
                "blob_read: truncated at meshlet_triangles array", "blob_read"});
            return false;
        }
    }

    // SDF magic + fields. We skip past SDF data without exposing it in the
    // view (Phase 1 validation doesn't need it).
    if (!c.expect_magic(0x20464453u)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.missing_sdf",
            "blob_read: missing or misplaced SDF magic", "blob_read"});
        return false;
    }
    // Skip SDF header: resolution[3] (12B) + bounds_min[3] (12B) + bounds_max[3] (12B)
    {
        const u8* sdf_header;
        if (!c.read_bytes(sdf_header, sizeof(u32) * 3 + sizeof(f32) * 6)) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_header",
                "blob_read: truncated at SDF header", "blob_read"});
            return false;
        }
    }
    u32 sdf_data_count;
    if (!c.read_u32(sdf_data_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_data_count",
            "blob_read: truncated at sdf_data_count", "blob_read"});
        return false;
    }
    if (sdf_data_count > 0) {
        const u8* skip;
        if (!c.read_bytes(skip, (size_t)sdf_data_count * sizeof(u16))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_data",
                "blob_read: truncated at SDF data", "blob_read"});
            return false;
        }
    }
    u32 sdf_voxels_count;
    if (!c.read_u32(sdf_voxels_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_voxels_count",
            "blob_read: truncated at sdf_voxels_count", "blob_read"});
        return false;
    }
    if (sdf_voxels_count > 0) {
        const u8* skip;
        if (!c.read_bytes(skip, (size_t)sdf_voxels_count * sizeof(u8))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_voxels",
                "blob_read: truncated at SDF voxels", "blob_read"});
            return false;
        }
    }
    u32 sdf_vector_count;
    if (!c.read_u32(sdf_vector_count)) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_vector_count",
            "blob_read: truncated at sdf_vector_count", "blob_read"});
        return false;
    }
    if (sdf_vector_count > 0) {
        const u8* skip;
        if (!c.read_bytes(skip, (size_t)sdf_vector_count * sizeof(u16))) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.truncated_sdf_vector",
                "blob_read: truncated at SDF vector_field", "blob_read"});
            return false;
        }
    }

    cursor_in = c.p;
    return true;
}

u32 WalkSceneBlob(const scene_data& data,
                  const std::function<void(u32, const PackedMeshView&)>& visitor,
                  utl::vector<ErrorReport>& errors) {
    if (!data.buffer || data.buffer_size == 0) {
        errors.emplace_back(ErrorReport{Severity::Warning, "blob_read.empty_buffer",
            "blob_read: scene_data.buffer is empty", "blob_read"});
        return 0;
    }

    const u8* p   = data.buffer;
    const u8* end = data.buffer + data.buffer_size;
    u32 visited    = 0;

    cursor c{p, end};
    u32 lod_group_count = 0;
    if (!skip_scene_header(c, &lod_group_count, errors)) return 0;

    for (u32 lg = 0; lg < lod_group_count; ++lg) {
        const u32 mesh_count = skip_lod_group_header(c, nullptr, errors);
        if (mesh_count == u32_invalid_id) return visited;
        if (mesh_count > 1000000) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_read.bad_mesh_count",
                "blob_read: mesh_count unreasonably large", "blob_read"});
            return visited;
        }
        for (u32 m = 0; m < mesh_count; ++m) {
            PackedMeshView view;
            if (!ReadNextPackedMesh(c.p, end, view, errors)) return visited;
            visitor(visited, view);
            ++visited;
        }
    }

    return visited;
}

// ---- Phase 2 (M12.2): ReadSceneHeader ---------------------------------------

bool ReadSceneHeader(const u8*& cursor_in, const u8* end, SceneHeader& out,
                     utl::vector<ErrorReport>& errors) {
    cursor c{cursor_in, end};
    if (!read_scene_header(c, out, errors)) return false;
    // Cursor sits at the first lod_group's name_size u32 — ready for the
    // caller to iterate skip_lod_group_header + ReadNextPackedMesh.
    cursor_in = c.p;
    return true;
}

bool ReadSceneHeader(const scene_data& data, SceneHeader& out,
                     utl::vector<ErrorReport>& errors) {
    if (!data.buffer || data.buffer_size == 0) {
        errors.emplace_back(ErrorReport{Severity::Warning, "blob_read.empty_buffer",
            "blob_read: scene_data.buffer is empty", "blob_read"});
        return false;
    }
    const u8* p = data.buffer;
    const u8* end = data.buffer + data.buffer_size;
    return ReadSceneHeader(p, end, out, errors);
}

}  // namespace primal::tools::pipeline
