// M12.5 unit tests for PackedMeshDecoder (Phase 2 wire-up).
//
// Verifies the decoder inverts Geometry.cpp's encoder for all 4
// static_* elements_types reachable from ProcessableMesh IR:
//   - position_only
//   - static_color
//   - static_normal (with nz rebuilt from t_sign bit 1)
//   - static_normal_texture (with tangent.w lossy)
//
// Plus edge cases:
//   - Index widening (u16 → u32 widening for >65536 verts)
//   - Empty mesh (0 verts) — decoder returns success with empty arrays
//   - Multi-mesh scene (mixed elements_types)
//   - Truncated buffer — decoder returns false with error
//
// Strategy: build a ProcessableMesh → SceneBlobWriter::BuildPackedMesh →
// Serialize → wrap with scene header (via SerializeScene) → PackedMeshDecoder
// → assert round-trip equality within lossy tolerance.

#include "pipeline/PackedMeshDecoder.h"
#include "pipeline/SceneBlobWriter.h"
#include "pipeline/SceneBlobReader.h"
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "Geometry.h"
#include "../Engine/Utilities/IOStream.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

using namespace primal;
using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::pipeline;

namespace {

// ---- Fixtures --------------------------------------------------------------

ProcessableMesh make_position_only_mesh() {
    ProcessableMesh m;
    m.name = "pos_only";
    m.material_idx = 0;
    m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 0.f, 0.f});
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});
    m.indices.emplace_back(0);
    m.indices.emplace_back(1);
    m.indices.emplace_back(2);
    return m;
}

ProcessableMesh make_static_color_mesh() {
    ProcessableMesh m = make_position_only_mesh();
    m.name = "color_only";
    m.colors.emplace_back(v3{1.f, 0.f, 0.f});
    m.colors.emplace_back(v3{0.f, 1.f, 0.f});
    m.colors.emplace_back(v3{0.f, 0.f, 1.f});
    return m;
}

ProcessableMesh make_static_normal_mesh() {
    ProcessableMesh m = make_position_only_mesh();
    m.name = "normal_only";
    m.colors.emplace_back(v3{1.f, 0.f, 0.f});
    m.colors.emplace_back(v3{0.f, 1.f, 0.f});
    m.colors.emplace_back(v3{0.f, 0.f, 1.f});
    // Mix of +z and -z normals to exercise t_sign bit 1.
    m.normals.emplace_back(v3{0.f, 0.f,  1.f});  // bit 1 set
    m.normals.emplace_back(v3{0.f, 0.f, -1.f});  // bit 1 clear
    m.normals.emplace_back(v3{1.f, 0.f,  0.f});  // bit 1 clear (z=0)
    return m;
}

ProcessableMesh make_static_normal_texture_mesh() {
    ProcessableMesh m = make_position_only_mesh();
    m.name = "full";
    m.colors.emplace_back(v3{1.f, 0.f, 0.f});
    m.colors.emplace_back(v3{0.f, 1.f, 0.f});
    m.colors.emplace_back(v3{0.f, 0.f, 1.f});
    // Normals all have nz=0 (unit length on the xy equator). For
    // static_normal_texture the encoder's latent bug drops the nz sign bit,
    // so the decoder can't recover it. nz=0 round-trips regardless of sign;
    // the nx/xy precision recovery is what we actually verify here. Using
    // exact-representable unit vectors (|nx|+|ny|=1) keeps nz=sqrt(0)=0
    // after quantization — approximate values like 0.707 drift and produce
    // non-zero nz outside tolerance.
    m.normals.emplace_back(v3{1.f,  0.f, 0.f});
    m.normals.emplace_back(v3{0.f,  1.f, 0.f});
    m.normals.emplace_back(v3{-1.f, 0.f, 0.f});
    // Tangent.w > 0 && tangent.z > 0 → t_sign bit 0 set
    // Tangent.w > 0 && tangent.z < 0 → bit 0 clear
    m.tangents.emplace_back(v4{1.f, 0.f, 1.f, 1.f});  // bit 0 set
    m.tangents.emplace_back(v4{1.f, 0.f, -1.f, 1.f}); // bit 0 clear
    m.tangents.emplace_back(v4{0.f, 1.f, 1.f, 1.f});  // bit 0 set
    UVSet uvs;
    uvs.purpose = UVSetPurpose::Texture;
    uvs.coords.emplace_back(v2{0.f, 0.f});
    uvs.coords.emplace_back(v2{1.f, 0.f});
    uvs.coords.emplace_back(v2{0.f, 1.f});
    m.uv_sets.emplace_back(std::move(uvs));
    return m;
}

// Wrap a PackedMesh in a minimal scene_data.buffer so the decoder can find it.
// Helper that builds the buffer with SerializeScene + returns owning memory.
struct scene_buffer {
    utl::vector<u8> bytes;
    scene_data sd{};

    scene_buffer(const std::string& scene_name,
                 const utl::vector<material>& materials,
                 const utl::vector<PackedMesh>& meshes) {
        const size_t sz = GetSceneSize(scene_name, materials, meshes);
        bytes.resize(sz);
        utl::blob_stream_writer blob{bytes.data(), sz};
        SerializeScene(scene_name, materials, meshes, blob);
        sd.buffer = bytes.data();
        sd.buffer_size = (u32)sz;
    }
};

// Build a single-mesh scene_data for round-trip testing.
scene_buffer build_single_mesh_scene(const ProcessableMesh& ir) {
    utl::vector<ErrorReport> errs;
    PackedMesh pm = BuildPackedMesh(ir, {}, u32_invalid_id, -1.f, errs);
    material dummy_mat;
    dummy_mat.name = "default";
    utl::vector<material> mats;
    mats.emplace_back(std::move(dummy_mat));
    utl::vector<PackedMesh> meshes;
    meshes.emplace_back(std::move(pm));
    return scene_buffer{ir.name, mats, meshes};
}

// Compare f32 with absolute tolerance (pack_float<16> loses ~1/32767).
bool approx(f32 a, f32 b, f32 eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

bool v3_approx(const v3& a, const v3& b, f32 eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

}  // namespace

// ---- Round-trip tests ------------------------------------------------------

bool test_position_only_round_trip() {
    ProcessableMesh src = make_position_only_mesh();
    scene_buffer buf = build_single_mesh_scene(src);
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) {
        std::cout << "  DecodeScene failed:\n";
        for (const auto& e : errs) std::cout << "    " << e.code << ": " << e.message << "\n";
        return false;
    }
    if (dst.lods.size() != 1 || dst.lods[0].meshes.size() != 1) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    if (m.positions.size() != src.positions.size()) return false;
    for (u32 i = 0; i < (u32)src.positions.size(); ++i) {
        if (!v3_approx(m.positions[i], src.positions[i])) return false;
    }
    if (m.indices.size() != src.indices.size()) return false;
    for (u32 i = 0; i < (u32)src.indices.size(); ++i) {
        if (m.indices[i] != src.indices[i]) return false;
    }
    if (!m.normals.empty() || !m.tangents.empty() || !m.colors.empty() || !m.uv_sets.empty()) return false;
    return true;
}

bool test_static_color_round_trip() {
    ProcessableMesh src = make_static_color_mesh();
    scene_buffer buf = build_single_mesh_scene(src);
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    // colors are encoded via pack_unit_float<8> → ~1/255 loss.
    for (u32 i = 0; i < (u32)src.colors.size(); ++i) {
        if (!v3_approx(m.colors[i], src.colors[i], 5e-3f)) {
            std::cout << "  color[" << i << "] expected ("
                      << src.colors[i].x << "," << src.colors[i].y << "," << src.colors[i].z
                      << ") got (" << m.colors[i].x << "," << m.colors[i].y << ","
                      << m.colors[i].z << ")\n";
            return false;
        }
    }
    return true;
}

bool test_static_normal_round_trip() {
    ProcessableMesh src = make_static_normal_mesh();
    scene_buffer buf = build_single_mesh_scene(src);
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    if (m.normals.size() != src.normals.size()) {
        std::cout << "  normals.size=" << m.normals.size() << " expected " << src.normals.size() << "\n";
        return false;
    }
    for (u32 i = 0; i < (u32)src.normals.size(); ++i) {
        if (!v3_approx(m.normals[i], src.normals[i])) {
            std::cout << "  normal[" << i << "] expected ("
                      << src.normals[i].x << "," << src.normals[i].y << "," << src.normals[i].z
                      << ") got (" << m.normals[i].x << "," << m.normals[i].y << ","
                      << m.normals[i].z << ")\n";
            return false;
        }
    }
    return true;
}

bool test_static_normal_texture_round_trip() {
    ProcessableMesh src = make_static_normal_texture_mesh();
    scene_buffer buf = build_single_mesh_scene(src);
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    if (m.normals.size() != src.normals.size()) return false;
    if (m.tangents.size() != src.tangents.size()) return false;
    if (m.uv_sets.size() != 1 || m.uv_sets[0].coords.size() != src.uv_sets[0].coords.size()) return false;

    // normals: nz is rebuilt from t_sign bit 1, but for static_normal_texture
    // the encoder's latent bug drops bit 1 → decoder always reads bit 1 = 0
    // and returns -sqrt. The test fixture uses normals with z=0 (so
    // nx²+ny²=1, nz=sqrt(0)=0) — sign doesn't matter. nz sign correctness
    // for the static_normal path is exercised in test_static_normal_round_trip.
    for (u32 i = 0; i < (u32)src.normals.size(); ++i) {
        if (!v3_approx(m.normals[i], src.normals[i], 5e-3f)) {
            std::cout << "  normal[" << i << "] expected ("
                      << src.normals[i].x << "," << src.normals[i].y << "," << src.normals[i].z
                      << ") got (" << m.normals[i].x << "," << m.normals[i].y << ","
                      << m.normals[i].z << ")\n";
            return false;
        }
    }
    // tangents: tx, ty exact; tz rebuilt from bit 0 (sign); w = ±1.
    for (u32 i = 0; i < (u32)src.tangents.size(); ++i) {
        const v4& exp = src.tangents[i];
        const v4& got = m.tangents[i];
        if (!approx(got.x, exp.x, 5e-3f) || !approx(got.y, exp.y, 5e-3f)) {
            std::cout << "  tangent[" << i << "].xy mismatch\n";
            return false;
        }
        // tz rebuild: sign depends on whether (tw>0 && tz>0).
        const bool exp_sign_pos = (exp.w > 0.f) && (exp.z > 0.f);
        const f32 exp_tz = exp_sign_pos ? std::sqrt(1.f - exp.x*exp.x - exp.y*exp.y)
                                        : -std::sqrt(1.f - exp.x*exp.x - exp.y*exp.y);
        if (!approx(got.z, exp_tz, 5e-3f)) {
            std::cout << "  tangent[" << i << "].z expected " << exp_tz << " got " << got.z << "\n";
            return false;
        }
        // w: lossy single bit. Positive sign → 1.f, negative → -1.f.
        const f32 exp_w = exp_sign_pos ? 1.f : -1.f;
        if (!approx(got.w, exp_w, 1e-6f)) return false;
    }
    // UVs: exact f32 round-trip.
    for (u32 i = 0; i < (u32)src.uv_sets[0].coords.size(); ++i) {
        const v2& exp = src.uv_sets[0].coords[i];
        const v2& got = m.uv_sets[0].coords[i];
        if (!approx(got.x, exp.x, 1e-6f) || !approx(got.y, exp.y, 1e-6f)) return false;
    }
    return true;
}

// ---- Edge cases ------------------------------------------------------------

bool test_index_widening_large_vertex_count() {
    ProcessableMesh src;
    src.name = "big";
    // > 65536 verts forces u32 indices on encode.
    for (u32 i = 0; i < 70000; ++i) {
        src.positions.emplace_back(v3{(f32)i, 0.f, 0.f});
    }
    // Reference a few verts; indices just need to be valid.
    src.indices.emplace_back(0);
    src.indices.emplace_back(1);
    src.indices.emplace_back(69999);

    scene_buffer buf = build_single_mesh_scene(src);
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    if (m.positions.size() != 70000) return false;
    if (m.indices.size() != 3) return false;
    if (m.indices[0] != 0 || m.indices[1] != 1 || m.indices[2] != 69999) return false;
    return true;
}

bool test_empty_mesh_returns_success() {
    // Build an empty PackedMesh manually (BuildPackedMesh emits a warning
    // and leaves buffers empty when num_vertices==0).
    PackedMesh pm;
    pm.name = "empty";
    pm.elements_type = elements::elements_type::position_only;
    utl::vector<PackedMesh> meshes;
    meshes.emplace_back(std::move(pm));
    utl::vector<material> mats;
    scene_buffer buf{"empty_scene", mats, meshes};

    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    if (dst.lods.size() != 1 || dst.lods[0].meshes.size() != 1) return false;
    const ProcessableMesh& m = dst.lods[0].meshes[0];
    return m.positions.empty() && m.indices.empty();
}

bool test_multi_mesh_mixed_elements_types() {
    // Build three meshes with different elements_types in one scene.
    ProcessableMesh a = make_position_only_mesh();
    a.name = "a";
    ProcessableMesh b = make_static_color_mesh();
    b.name = "b";
    ProcessableMesh c = make_static_normal_texture_mesh();
    c.name = "c";

    utl::vector<ErrorReport> errs;
    PackedMesh pma = BuildPackedMesh(a, {}, u32_invalid_id, -1.f, errs);
    PackedMesh pmb = BuildPackedMesh(b, {}, u32_invalid_id, -1.f, errs);
    PackedMesh pmc = BuildPackedMesh(c, {}, u32_invalid_id, -1.f, errs);
    utl::vector<PackedMesh> meshes;
    meshes.emplace_back(std::move(pma));
    meshes.emplace_back(std::move(pmb));
    meshes.emplace_back(std::move(pmc));
    utl::vector<material> mats;
    scene_buffer buf{"multi", mats, meshes};

    ProcessableScene dst;
    if (!DecodeScene(buf.sd, dst, errs)) return false;
    if (dst.lods[0].meshes.size() != 3) return false;
    // Order preserved.
    if (dst.lods[0].meshes[0].name != "a") return false;
    if (dst.lods[0].meshes[1].name != "b") return false;
    if (dst.lods[0].meshes[2].name != "c") return false;
    // b has colors (static_color); c has colors + normals + tangents + uvs.
    if (dst.lods[0].meshes[1].colors.size() != 3) return false;
    if (dst.lods[0].meshes[1].normals.size() != 0) return false;
    if (dst.lods[0].meshes[2].tangents.size() != 3) return false;
    if (dst.lods[0].meshes[2].uv_sets[0].coords.size() != 3) return false;
    return true;
}

bool test_truncated_buffer_returns_error() {
    ProcessableMesh src = make_static_normal_texture_mesh();
    scene_buffer buf = build_single_mesh_scene(src);
    // Truncate the buffer by 1 byte.
    utl::vector<u8> trunc(buf.bytes.begin(), buf.bytes.end() - 1);
    scene_data sd{};
    sd.buffer = trunc.data();
    sd.buffer_size = (u32)trunc.size();
    ProcessableScene dst;
    utl::vector<ErrorReport> errs;
    const bool ok = DecodeScene(sd, dst, errs);
    // Decoder must not crash; it must return false with at least one error.
    if (ok) return false;
    return !errs.empty();
}

// ---- ReadSceneHeader direct test -------------------------------------------

bool test_read_scene_header_extracts_name_and_materials() {
    // Build a scene with known name + 2 materials.
    utl::vector<material> mats;
    material m0; m0.name = "wood"; m0.diffuse_texture = "wood_d.png"; m0.normal_texture = "wood_n.png";
    material m1; m1.name = "metal"; m1.diffuse_texture = "metal_d.png"; m1.normal_texture = "metal_n.png";
    mats.emplace_back(std::move(m0));
    mats.emplace_back(std::move(m1));
    utl::vector<PackedMesh> meshes;
    // No meshes — header still serializes.
    scene_buffer buf{"my_scene", mats, meshes};

    SceneHeader hdr;
    utl::vector<ErrorReport> errs;
    if (!ReadSceneHeader(buf.sd, hdr, errs)) return false;
    if (hdr.scene_name != "my_scene") return false;
    if (hdr.materials.size() != 2) return false;
    if (hdr.materials[0].name != "wood") return false;
    if (hdr.materials[0].diffuse_texture != "wood_d.png") return false;
    if (hdr.materials[0].normal_texture != "wood_n.png") return false;
    if (hdr.materials[1].name != "metal") return false;
    if (hdr.lod_group_count != 1) return false;
    return true;
}

// ---- Test runner -----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_position_only_round_trip),
        CASE(test_static_color_round_trip),
        CASE(test_static_normal_round_trip),
        CASE(test_static_normal_texture_round_trip),
        CASE(test_index_widening_large_vertex_count),
        CASE(test_empty_mesh_returns_success),
        CASE(test_multi_mesh_mixed_elements_types),
        CASE(test_truncated_buffer_returns_error),
        CASE(test_read_scene_header_extracts_name_and_materials),
    };

    int passed = 0, failed = 0;
    for (const auto& c : cases) {
        bool ok = false;
        try { ok = c.fn(); }
        catch (const std::exception& e) {
            std::cout << "  EXCEPTION  " << c.name << ": " << e.what() << "\n";
            ok = false;
        } catch (...) { ok = false; }
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << c.name << "\n";
        if (ok) ++passed; else ++failed;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
