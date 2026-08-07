#include "SceneBlobWriter.h"

#include <cmath>
#include <cstring>

namespace primal::tools::pipeline {
namespace {

using elements::elements_type;

// Mirror of Geometry.cpp:223 get_vertex_element_size. Reimplemented locally
// to avoid exposing it from Geometry.h — and because Phase 1 only supports
// static_* types anyway.
u64 get_vertex_element_size(elements_type::type t) {
    switch (t) {
    case elements_type::static_color:           return sizeof(elements::static_color);
    case elements_type::static_normal:          return sizeof(elements::static_normal);
    case elements_type::static_normal_texture:  return sizeof(elements::static_normal_texture);
    // Skeletal types are unreachable from ProcessableMesh IR. Return 0 lets
    // an accidental skeletal code path fail loudly at the element_buffer
    // resize assert rather than silently corrupting memory.
    default:                                    return 0;
    }
}

// f32 in [-1,1] → u16 (pack_float<16> lives in Engine/Utilities/Math.h but
// is already pulled in via ToolsCommon.h).
u16 pack_dir(f32 f) {
    if (!std::isfinite(f)) f = 0.f;
    if (f < -1.f) f = -1.f;
    if (f >  1.f) f =  1.f;
    return (u16)math::pack_float<16>(f, -1.f, 1.f);
}

// f32 in [0,1] → u8 (mirrors pack_unit_float<8> used by pack_vertices).
u8 pack_color(f32 c) {
    if (!std::isfinite(c)) c = 0.f;
    if (c < 0.f) c = 0.f;
    if (c > 1.f) c = 1.f;
    return (u8)math::pack_unit_float<8>(c);
}

}  // namespace

// ---- determine_elements_type ---------------------------------------------

elements::elements_type::type determine_elements_type(const ProcessableMesh& m) {
    const bool has_normals  = !m.normals.empty();
    const bool has_tangents = !m.tangents.empty();
    const UVSet* tex_uv = find_uv_set(m, UVSetPurpose::Texture);
    const bool has_tex_uv = tex_uv && !tex_uv->coords.empty();

    if (has_normals && has_tangents && has_tex_uv) return elements_type::static_normal_texture;
    if (has_normals)                                return elements_type::static_normal;
    if (!m.colors.empty())                          return elements_type::static_color;
    return elements_type::position_only;
}

// ---- BuildPackedMesh -----------------------------------------------------

PackedMesh BuildPackedMesh(const ProcessableMesh& m,
                           const MeshletData& meshlets,
                           u32 lod_id, f32 lod_threshold,
                           utl::vector<ErrorReport>& errors) {
    PackedMesh pm;
    pm.name           = m.name;
    pm.lod_id         = lod_id;
    pm.material_idx   = m.material_idx;
    pm.lod_threshold  = lod_threshold;
    pm.elements_type  = determine_elements_type(m);
    pm.indices        = m.indices;
    pm.meshlets       = meshlets;

    const u32 num_vertices = (u32)m.positions.size();
    if (num_vertices == 0) {
        errors.emplace_back(ErrorReport{
            Severity::Warning, "blob.empty_mesh",
            "blob: empty mesh, no buffers written", "blob"});
        return pm;
    }

    // ---- position_buffer (12 B / vert) ---------------------------------
    pm.position_buffer.resize(12 * num_vertices);
    {
        f32* p = reinterpret_cast<f32*>(pm.position_buffer.data());
        for (u32 i = 0; i < num_vertices; ++i) {
            p[i * 3 + 0] = m.positions[i].x;
            p[i * 3 + 1] = m.positions[i].y;
            p[i * 3 + 2] = m.positions[i].z;
        }
    }

    // ---- element_buffer (per elements_type) ----------------------------
    const u64 elem_size = get_vertex_element_size(pm.elements_type);
    pm.element_buffer.resize(elem_size * num_vertices);

    const UVSet* tex_uv = find_uv_set(m, UVSetPurpose::Texture);

    switch (pm.elements_type) {
    case elements_type::static_color: {
        auto* eb = reinterpret_cast<elements::static_color*>(pm.element_buffer.data());
        for (u32 i = 0; i < num_vertices; ++i) {
            const math::v3 c = (i < m.colors.size()) ? m.colors[i] : math::v3{0.f,0.f,0.f};
            eb[i].color[0] = pack_color(c.x);
            eb[i].color[1] = pack_color(c.y);
            eb[i].color[2] = pack_color(c.z);
            eb[i].pad = 0;
        }
        break;
    }
    case elements_type::static_normal: {
        auto* eb = reinterpret_cast<elements::static_normal*>(pm.element_buffer.data());
        for (u32 i = 0; i < num_vertices; ++i) {
            const math::v3 c = (i < m.colors.size()) ? m.colors[i] : math::v3{0.f,0.f,0.f};
            eb[i].color[0] = pack_color(c.x);
            eb[i].color[1] = pack_color(c.y);
            eb[i].color[2] = pack_color(c.z);
            // Replicate pack_vertices:339-355: static_normal sets t_sign to
            // (normal.z > 0) << 1 (bit 1 only).
            const math::v3 n = (i < m.normals.size()) ? m.normals[i] : math::v3{0.f,1.f,0.f};
            eb[i].t_sign = (u8)((n.z > 0.f) << 1);
            eb[i].normal[0] = pack_dir(n.x);
            eb[i].normal[1] = pack_dir(n.y);
        }
        break;
    }
    case elements_type::static_normal_texture: {
        auto* eb = reinterpret_cast<elements::static_normal_texture*>(pm.element_buffer.data());
        for (u32 i = 0; i < num_vertices; ++i) {
            const math::v3 c = (i < m.colors.size()) ? m.colors[i] : math::v3{0.f,0.f,0.f};
            eb[i].color[0] = pack_color(c.x);
            eb[i].color[1] = pack_color(c.y);
            eb[i].color[2] = pack_color(c.z);
            const math::v3 n = (i < m.normals.size()) ? m.normals[i] : math::v3{0.f,1.f,0.f};
            const math::v4 t = (i < m.tangents.size()) ? m.tangents[i] : math::v4{0.f,1.f,0.f,1.f};
            // Replicate pack_vertices:358-376 latent bug: static_normal_texture
            // OVERWRITES t_sign set by static_normal branch, dropping the
            // normal.z bit and keeping only the tangent handedness × tangent.z bit.
            // This MUST be preserved for byte-for-byte compatibility with the
            // 4 downstream consumers (ContentToEngine.cpp / pack_geometry.py /
            // MeshletValidator.py / C# PrimalEditor).
            eb[i].t_sign = (u8)((t.w > 0.f) && (t.z > 0.f));
            eb[i].normal[0]  = pack_dir(n.x);
            eb[i].normal[1]  = pack_dir(n.y);
            eb[i].tangent[0] = pack_dir(t.x);
            eb[i].tangent[1] = pack_dir(t.y);
            // UV — pulled from the first Texture-purpose UVSet. If absent,
            // write zeros (still byte-valid, downstream readers will sample
            // neutral and the asset is visually broken but parseable).
            if (tex_uv && i < (u32)tex_uv->coords.size()) {
                eb[i].uv = tex_uv->coords[i];
            } else {
                eb[i].uv = math::v2{0.f, 0.f};
            }
        }
        break;
    }
    default:
        // position_only → element_buffer stays empty (elem_size was 0).
        // Skeletal unreachable from ProcessableMesh.
        break;
    }

    return pm;
}

// ---- GetPackedMeshSize ---------------------------------------------------

size_t GetPackedMeshSize(const PackedMesh& pm) {
    size_t size = 0;
    size += sizeof(u32) + pm.name.size();                // name + length
    size += sizeof(u32);                                 // lod_id
    size += sizeof(u32);                                 // material_idx
    size += sizeof(u32);                                 // elements_size
    size += sizeof(u32);                                 // elements_type
    const u32 num_vertices = (u32)(pm.position_buffer.size() / 12);
    size += sizeof(u32);                                 // num_vertices
    size += sizeof(u32);                                 // index_size
    const u32 num_indices = (u32)pm.indices.size();
    const u32 index_size = (num_vertices < (1u << 16)) ? 2 : 4;
    size += sizeof(u32);                                 // num_indices
    size += sizeof(f32);                                 // lod_threshold
    size += pm.position_buffer.size();                   // position_buffer
    size += pm.element_buffer.size();                    // element_buffer
    size += (size_t)index_size * num_indices;            // indices
    size += sizeof(u32);                                 // MSHL magic
    size += sizeof(u32) + pm.meshlets.meshlets.size()    * sizeof(mesh::meshlet);
    size += sizeof(u32) + pm.meshlets.meshlet_vertices.size() * sizeof(u32);
    size += sizeof(u32) + pm.meshlets.meshlet_triangles.size() * sizeof(u8);
    size += sizeof(u32);                                 // SDF magic
    size += sizeof(u32) * 3 + sizeof(f32) * 3 + sizeof(f32) * 3;  // res + bmin + bmax
    size += sizeof(u32) + pm.sdf.data.size()        * sizeof(u16);
    size += sizeof(u32) + pm.sdf.voxels.size()      * sizeof(u8);
    size += sizeof(u32) + pm.sdf.vector_field.size() * sizeof(u16);
    return size;
}

// ---- Serialize -----------------------------------------------------------

void Serialize(const PackedMesh& pm, utl::blob_stream_writer& blob) {
    // Header
    blob.write((u32)pm.name.size());
    blob.write(pm.name.c_str(), pm.name.size());
    blob.write(pm.lod_id);
    blob.write(pm.material_idx);
    const u32 elements_size = (u32)get_vertex_element_size(pm.elements_type);
    blob.write(elements_size);
    blob.write((u32)pm.elements_type);
    const u32 num_vertices = (u32)(pm.position_buffer.size() / 12);
    blob.write(num_vertices);
    const u32 index_size = (num_vertices < (1u << 16)) ? (u32)sizeof(u16) : (u32)sizeof(u32);
    blob.write(index_size);
    const u32 num_indices = (u32)pm.indices.size();
    blob.write(num_indices);
    blob.write(pm.lod_threshold);

    // Buffers
    blob.write(pm.position_buffer.data(), pm.position_buffer.size());
    blob.write(pm.element_buffer.data(),  pm.element_buffer.size());

    // Indices (u16 if vertex count fits, else u32).
    if (index_size == sizeof(u16)) {
        utl::vector<u16> indices16(num_indices);
        for (u32 i = 0; i < num_indices; ++i) indices16[i] = (u16)pm.indices[i];
        blob.write((const u8*)indices16.data(), (size_t)index_size * num_indices);
    } else {
        blob.write((const u8*)pm.indices.data(), (size_t)index_size * num_indices);
    }

    // MSHL magic + meshlets
    constexpr u32 magic_mshl = 0x4C48534D;  // "MSHL"
    blob.write(magic_mshl);

    blob.write((u32)pm.meshlets.meshlets.size());
    if (!pm.meshlets.meshlets.empty()) {
        blob.write((const u8*)pm.meshlets.meshlets.data(),
                   pm.meshlets.meshlets.size() * sizeof(mesh::meshlet));
    }
    blob.write((u32)pm.meshlets.meshlet_vertices.size());
    if (!pm.meshlets.meshlet_vertices.empty()) {
        blob.write((const u8*)pm.meshlets.meshlet_vertices.data(),
                   pm.meshlets.meshlet_vertices.size() * sizeof(u32));
    }
    blob.write((u32)pm.meshlets.meshlet_triangles.size());
    if (!pm.meshlets.meshlet_triangles.empty()) {
        blob.write((const u8*)pm.meshlets.meshlet_triangles.data(),
                   pm.meshlets.meshlet_triangles.size() * sizeof(u8));
    }

    // SDF magic + sdf_data
    constexpr u32 magic_sdf = 0x20464453;  // "SDF "
    blob.write(magic_sdf);

    blob.write((const u8*)pm.sdf.resolution,  sizeof(u32) * 3);
    blob.write((const u8*)pm.sdf.bounds_min,  sizeof(f32) * 3);
    blob.write((const u8*)pm.sdf.bounds_max,  sizeof(f32) * 3);

    blob.write((u32)pm.sdf.data.size());
    if (!pm.sdf.data.empty()) {
        blob.write((const u8*)pm.sdf.data.data(),
                   pm.sdf.data.size() * sizeof(u16));
    }
    blob.write((u32)pm.sdf.voxels.size());
    if (!pm.sdf.voxels.empty()) {
        blob.write((const u8*)pm.sdf.voxels.data(),
                   pm.sdf.voxels.size() * sizeof(u8));
    }
    blob.write((u32)pm.sdf.vector_field.size());
    if (!pm.sdf.vector_field.empty()) {
        blob.write((const u8*)pm.sdf.vector_field.data(),
                   pm.sdf.vector_field.size() * sizeof(u16));
    }
}

}  // namespace primal::tools::pipeline
