#include "PackedMeshDecoder.h"

#include <cmath>
#include <cstring>

namespace primal::tools::pipeline {
namespace {

using elements::elements_type;

// Read a little-endian u16 from a byte pointer. No bounds check here —
// caller guarantees the read is in-range (verified via elements_size ×
// num_vertices before invoking per-vertex decode).
u16 read_u16(const u8* p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

u32 read_u32_le(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

f32 read_f32_le(const u8* p) {
    u32 bits = read_u32_le(p);
    f32 f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Invert pack_float<16>(value, -1, 1). See Engine/Utilities/Math.h:60-64.
// The encoder clamped to [-1, 1] before packing; round-trip loses <1/32767.
f32 unpack_dir(u16 bits) {
    return math::unpack_to_float<16>((u32)bits, -1.f, 1.f);
}

// Invert pack_unit_float<8>(value) for color channels. See Math.h:42-48.
f32 unpack_color(u8 c) {
    return math::unpack_to_unit_float<8>((u32)c);
}

// Reconstruct the missing Z component of a unit vector given X, Y, and a
// sign bit. sign_pos=true → +sqrt(1-x²-y²), false → -sqrt(...). Clamps the
// radicand to >=0 to handle quantization drift pushing x²+y² slightly >1.
f32 reconstruct_z(f32 x, f32 y, bool sign_pos) {
    f32 r2 = 1.f - x * x - y * y;
    if (r2 < 0.f) r2 = 0.f;
    const f32 z = std::sqrt(r2);
    return sign_pos ? z : -z;
}

// Decode position_buffer (12 B/vert → 3 × f32 LE).
bool decode_positions(const PackedMeshView& v, ProcessableMesh& dst) {
    const u32 n = v.num_vertices;
    dst.positions.resize(n);
    const u8* pb = v.position_buffer;
    for (u32 i = 0; i < n; ++i) {
        const u8* p = pb + i * 12;
        dst.positions[i] = math::v3{read_f32_le(p), read_f32_le(p + 4), read_f32_le(p + 8)};
    }
    return true;
}

// Decode indices. u16 widened to u32; u32 passed through.
bool decode_indices(const PackedMeshView& v, ProcessableMesh& dst,
                    utl::vector<ErrorReport>& errors) {
    if (v.index_size != 2 && v.index_size != 4) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.bad_index_size",
            "blob_decode: index_size must be 2 or 4, got " + std::to_string(v.index_size),
            "blob_decode"});
        return false;
    }
    dst.indices.resize(v.num_indices);
    const u8* ib = v.index_buffer;
    if (v.index_size == sizeof(u16)) {
        for (u32 i = 0; i < v.num_indices; ++i) {
            dst.indices[i] = (u32)read_u16(ib + i * 2);
        }
    } else {
        for (u32 i = 0; i < v.num_indices; ++i) {
            dst.indices[i] = read_u32_le(ib + i * 4);
        }
    }
    return true;
}

// position_only: no element_buffer. Decode just returns (positions + indices
// already filled by caller).
bool decode_position_only(const PackedMeshView& /*v*/, ProcessableMesh& /*dst*/) {
    return true;
}

// static_color (4 B/vert): color[3] + pad.
bool decode_static_color(const PackedMeshView& v, ProcessableMesh& dst) {
    const u32 n = v.num_vertices;
    dst.colors.resize(n);
    const u8* eb = v.element_buffer;
    for (u32 i = 0; i < n; ++i) {
        const u8* p = eb + i * v.elements_size;
        dst.colors[i] = math::v3{
            unpack_color(p[0]),
            unpack_color(p[1]),
            unpack_color(p[2]),
        };
    }
    return true;
}

// static_normal (8 B/vert): color[3] + t_sign + normal[2].
// t_sign bit 1 (mask 0x02) = (normal.z > 0).
bool decode_static_normal(const PackedMeshView& v, ProcessableMesh& dst) {
    const u32 n = v.num_vertices;
    dst.colors.resize(n);
    dst.normals.resize(n);
    const u8* eb = v.element_buffer;
    for (u32 i = 0; i < n; ++i) {
        const u8* p = eb + i * v.elements_size;
        dst.colors[i] = math::v3{
            unpack_color(p[0]),
            unpack_color(p[1]),
            unpack_color(p[2]),
        };
        const u8 t_sign = p[3];
        const f32 nx = unpack_dir(read_u16(p + 4));
        const f32 ny = unpack_dir(read_u16(p + 6));
        const bool sign_pos = (t_sign & 0x02) != 0;
        const f32 nz = reconstruct_z(nx, ny, sign_pos);
        dst.normals[i] = math::v3{nx, ny, nz};
    }
    return true;
}

// static_normal_texture (sizeof 24 B on macOS due to math::v2 8-byte align;
// could be 20 B on platforms where math::v2 has 4-byte align). Stride is
// read from view.elements_size, so it adapts to whichever platform encoded
// the blob.
//
// Field byte offsets (per the Geometry.h struct):
//   0-2:   color[3]
//   3:     t_sign
//   4-5:   normal[0]
//   6-7:   normal[1]
//   8-9:   tangent[0]
//   10-11: tangent[1]
//   (4 B padding on macOS only — math::v2 is 8-byte aligned)
//   16-23: uv (f32 × 2)
//
// On a 20-byte-layout platform, uv sits at offset 12 with no padding. We
// compute the uv offset as elements_size - sizeof(f32) * 2 to handle both
// layouts — uv is always the LAST 8 bytes of the struct.
//
// t_sign bit semantics (encoded by pack_vertices:358-376):
//   bit 0 (mask 0x01): (tangent.w > 0) && (tangent.z > 0)
//   bit 1 (mask 0x02): NEVER SET for static_normal_texture — encoder bug
//     overwrites t_sign entirely in the static_normal_texture branch,
//     dropping the normal.z bit. Decoder reads bit 1 = 0 and returns
//     -sqrt(1-nx²-ny²). Caller should run derive::Run to rebuild normals
//     correctly.
bool decode_static_normal_texture(const PackedMeshView& v, ProcessableMesh& dst) {
    const u32 n = v.num_vertices;
    dst.colors.resize(n);
    dst.normals.resize(n);
    dst.tangents.resize(n);
    dst.uv_sets.resize(1);
    dst.uv_sets[0].purpose = UVSetPurpose::Texture;
    dst.uv_sets[0].coords.resize(n);

    const u8* eb = v.element_buffer;
    // uv is always the last 8 bytes of the per-vertex stride.
    const size_t uv_off = (size_t)v.elements_size - sizeof(f32) * 2;
    for (u32 i = 0; i < n; ++i) {
        const u8* p = eb + i * v.elements_size;
        dst.colors[i] = math::v3{
            unpack_color(p[0]),
            unpack_color(p[1]),
            unpack_color(p[2]),
        };
        const u8 t_sign = p[3];
        // Normal: encoder bug drops bit 1, so bit 1 reads as 0 → nz = -sqrt.
        const f32 nx = unpack_dir(read_u16(p + 4));
        const f32 ny = unpack_dir(read_u16(p + 6));
        const bool normal_sign_pos = (t_sign & 0x02) != 0;
        const f32 nz = reconstruct_z(nx, ny, normal_sign_pos);
        dst.normals[i] = math::v3{nx, ny, nz};

        // Tangent: bit 0 = (tw > 0) && (tz > 0). Single bit collapses two signs.
        const f32 tx = unpack_dir(read_u16(p + 8));
        const f32 ty = unpack_dir(read_u16(p + 10));
        const bool tangent_sign_pos = (t_sign & 0x01) != 0;
        const f32 tz = reconstruct_z(tx, ty, tangent_sign_pos);
        const f32 tw = tangent_sign_pos ? 1.f : -1.f;
        dst.tangents[i] = math::v4{tx, ty, tz, tw};

        dst.uv_sets[0].coords[i] = math::v2{
            read_f32_le(p + uv_off),
            read_f32_le(p + uv_off + 4),
        };
    }
    return true;
}

}  // namespace

// ---- DecodePackedMesh -------------------------------------------------------

bool DecodePackedMesh(const PackedMeshView& v, ProcessableMesh& dst,
                      utl::vector<ErrorReport>& errors) {
    dst = ProcessableMesh{};
    dst.name         = v.name;
    dst.material_idx = v.material_idx;

    if (v.num_vertices == 0) {
        // Empty mesh — pipeline tolerates this (BuildPackedMesh emits a warning).
        // Return true with empty arrays; caller can decide policy.
        return true;
    }

    // Sanity-check the elements_type. Skeletal_* are unreachable from IR
    // (ProcessableMesh has no joint fields), so reject them loudly.
    switch (v.elements_type) {
    case elements_type::position_only:
    case elements_type::static_color:
    case elements_type::static_normal:
    case elements_type::static_normal_texture:
        break;
    default:
        errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.unsupported_elements_type",
            "blob_decode: skeletal_* elements_type (" + std::to_string((u32)v.elements_type) +
            ") is unreachable from ProcessableMesh IR", "blob_decode"});
        return false;
    }

    // Sanity-check buffers are present.
    if (!v.position_buffer) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.null_position",
            "blob_decode: position_buffer is null", "blob_decode"});
        return false;
    }
    if (v.elements_type != elements_type::position_only && !v.element_buffer) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.null_element",
            "blob_decode: element_buffer is null for elements_type " +
            std::to_string((u32)v.elements_type), "blob_decode"});
        return false;
    }
    if (v.num_indices > 0 && !v.index_buffer) {
        errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.null_index",
            "blob_decode: index_buffer is null but num_indices=" +
            std::to_string(v.num_indices), "blob_decode"});
        return false;
    }

    if (!decode_positions(v, dst)) return false;
    if (!decode_indices(v, dst, errors)) return false;

    switch (v.elements_type) {
    case elements_type::position_only:
        return decode_position_only(v, dst);
    case elements_type::static_color:
        return decode_static_color(v, dst);
    case elements_type::static_normal:
        return decode_static_normal(v, dst);
    case elements_type::static_normal_texture:
        return decode_static_normal_texture(v, dst);
    default:
        return false;  // unreachable — switch above already rejected
    }
}

// ---- DecodeScene ------------------------------------------------------------

bool DecodeScene(const scene_data& src, ProcessableScene& dst,
                 utl::vector<ErrorReport>& errors) {
    dst = ProcessableScene{};

    if (!src.buffer || src.buffer_size == 0) {
        errors.emplace_back(ErrorReport{Severity::Warning, "blob_decode.empty_buffer",
            "blob_decode: scene_data.buffer is empty", "blob_decode"});
        return false;
    }

    // Phase 1: parse scene wrapper (scene_name + materials + lod_group_count).
    SceneHeader header;
    const u8* cursor = src.buffer;
    const u8* end = src.buffer + src.buffer_size;
    if (!ReadSceneHeader(cursor, end, header, errors)) return false;

    dst.name      = std::move(header.scene_name);
    dst.materials = std::move(header.materials);

    // Phase 2: walk lod_groups + meshes, flattening all meshes into lods[0].
    // Phase 2 ProcessableScene treats all decoded meshes as LOD 0 source.
    ProcessableLod lod0;
    lod0.screen_threshold = 0.5f;  // AssetPipeline default for generated LODs

    for (u32 lg = 0; lg < header.lod_group_count; ++lg) {
        // skip_lod_group_header lives in the .cpp's anonymous namespace; we
        // re-implement the minimal skip here since it's not exposed publicly.
        u32 lod_name_size;
        if (cursor + 4 > end) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.truncated_lod_name",
                "blob_decode: truncated at lod_name_size", "blob_decode"});
            return false;
        }
        lod_name_size = read_u32_le(cursor);
        cursor += 4;
        if (lod_name_size > 4096) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.bad_lod_name_size",
                "blob_decode: lod_name_size unreasonably large", "blob_decode"});
            return false;
        }
        if (cursor + lod_name_size > end) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.truncated_lod_name_bytes",
                "blob_decode: truncated at lod_name bytes", "blob_decode"});
            return false;
        }
        cursor += lod_name_size;  // discard lod_group name — IR has no concept

        if (cursor + 4 > end) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.truncated_mesh_count",
                "blob_decode: truncated at mesh_count", "blob_decode"});
            return false;
        }
        const u32 mesh_count = read_u32_le(cursor);
        cursor += 4;
        if (mesh_count > 1000000) {
            errors.emplace_back(ErrorReport{Severity::Error, "blob_decode.bad_mesh_count",
                "blob_decode: mesh_count unreasonably large", "blob_decode"});
            return false;
        }

        for (u32 m = 0; m < mesh_count; ++m) {
            PackedMeshView view;
            if (!ReadNextPackedMesh(cursor, end, view, errors)) return false;
            ProcessableMesh pm;
            if (!DecodePackedMesh(view, pm, errors)) return false;
            lod0.meshes.emplace_back(std::move(pm));
        }
    }

    dst.lods.emplace_back(std::move(lod0));
    return true;
}

}  // namespace primal::tools::pipeline
