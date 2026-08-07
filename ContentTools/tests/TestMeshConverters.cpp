// M2.6 unit tests for ProcessableMesh IR + MeshConverters + validate_invariants.
//
// Verifies the IR round-trip contract: legacy `mesh` SoA → ProcessableMesh IR
// → legacy `mesh` SoA preserves all IR fields. validate_invariants must
// accept the well-formed case and reject each documented violation.
//
// Build/run: this target is wired into ContentTools/tests/CMakeLists.txt.
// Standalone (no Engine runtime dependency) — uses only header types plus
// the symbols ContentTools.dylib already exposes from common/*.cpp.

#include "../common/ProcessableMesh.h"
#include "../common/MeshConverters.h"
#include "../Geometry.h"

#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;

namespace {

// Element-wise comparison helpers — utl::vector has no operator==.
bool vec_eq_u32(const primal::utl::vector<u32>& a, const primal::utl::vector<u32>& b) {
    if (a.size() != b.size()) return false;
    for (u32 i = 0; i < (u32)a.size(); ++i) if (a[i] != b[i]) return false;
    return true;
}
bool vec_eq_v3(const primal::utl::vector<v3>& a, const primal::utl::vector<v3>& b) {
    if (a.size() != b.size()) return false;
    for (u32 i = 0; i < (u32)a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    }
    return true;
}
bool vec_eq_v4(const primal::utl::vector<v4>& a, const primal::utl::vector<v4>& b) {
    if (a.size() != b.size()) return false;
    for (u32 i = 0; i < (u32)a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y ||
            a[i].z != b[i].z || a[i].w != b[i].w) return false;
    }
    return true;
}
bool vec_eq_v2(const primal::utl::vector<v2>& a, const primal::utl::vector<v2>& b) {
    if (a.size() != b.size()) return false;
    for (u32 i = 0; i < (u32)a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
    }
    return true;
}

// Builds a tiny but fully-populated legacy mesh for round-trip testing.
// 4 vertices, 2 triangles, 1 UV set, normals + tangents + colors.
mesh make_legacy_fixture() {
    mesh m;
    m.name = "fixture";
    m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 1.f, 0.f});
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});
    for (u32 i = 0; i < 6; ++i) m.raw_indices.emplace_back(0);
    m.raw_indices[0] = 0; m.raw_indices[1] = 1; m.raw_indices[2] = 2;
    m.raw_indices[3] = 0; m.raw_indices[4] = 2; m.raw_indices[5] = 3;
    for (u32 i = 0; i < 4; ++i) m.normals.emplace_back(v3{0.f, 0.f, 1.f});
    for (u32 i = 0; i < 4; ++i) m.tangents.emplace_back(v4{1.f, 0.f, 0.f, 1.f});
    for (u32 i = 0; i < 4; ++i) m.colors.emplace_back(v3{1.f, 0.f, 0.f});
    m.uv_sets.resize(1);
    m.uv_sets[0].emplace_back(v2{0.f, 0.f});
    m.uv_sets[0].emplace_back(v2{1.f, 0.f});
    m.uv_sets[0].emplace_back(v2{1.f, 1.f});
    m.uv_sets[0].emplace_back(v2{0.f, 1.f});
    m.material_idx = 7;
    return m;
}

}  // namespace

// --- validate_invariants ---------------------------------------------------

bool test_validate_ok_complete() {
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    for (u32 i = 0; i < 3; ++i) m.normals.emplace_back(v3{0.f, 0.f, 1.f});
    for (u32 i = 0; i < 3; ++i) m.tangents.emplace_back(v4{1.f, 0.f, 0.f, 1.f});
    for (u32 i = 0; i < 3; ++i) m.colors.emplace_back(v3{1.f, 0.f, 0.f});
    UVSet s; s.purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < 3; ++i) s.coords.emplace_back(v2{0.f, 0.f});
    m.uv_sets.emplace_back(std::move(s));
    return validate_invariants(m);
}

bool test_validate_ok_minimal() {
    // Only positions + indices. All auxiliary arrays empty.
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    return validate_invariants(m);
}

bool test_validate_fail_empty_positions() {
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    return !validate_invariants(m);
}

bool test_validate_fail_index_count() {
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    m.indices.emplace_back(0);
    m.indices.emplace_back(1);          // only 2, not divisible by 3
    return !validate_invariants(m);
}

bool test_validate_fail_normal_count() {
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    for (u32 i = 0; i < 2; ++i) m.normals.emplace_back(v3{0.f, 0.f, 1.f});   // 2 != 3
    return !validate_invariants(m);
}

bool test_validate_fail_uv_count() {
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    UVSet s; s.purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < 2; ++i) s.coords.emplace_back(v2{0.f, 0.f});   // 2 != 3
    m.uv_sets.emplace_back(std::move(s));
    return !validate_invariants(m);
}

bool test_validate_uv_empty_ok() {
    // UVSet with zero coords is always OK regardless of vertex count.
    ProcessableMesh m;
    for (u32 i = 0; i < 3; ++i) m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) m.indices.emplace_back(i);
    UVSet s; s.purpose = UVSetPurpose::Texture;   // coords empty
    m.uv_sets.emplace_back(std::move(s));
    return validate_invariants(m);
}

// --- to_processable / from_processable round-trip -------------------------

bool test_to_processable_copies_ir_fields() {
    mesh src = make_legacy_fixture();
    ProcessableMesh dst;
    to_processable(src, dst);

    if (dst.name != "fixture") return false;
    if (dst.positions.size() != 4) return false;
    if (dst.indices.size() != 6) return false;
    if (dst.indices[0] != 0 || dst.indices[dst.indices.size() - 1] != 3) return false;
    if (dst.normals.size() != 4) return false;
    if (dst.tangents.size() != 4) return false;
    if (dst.colors.size() != 4) return false;
    if (dst.uv_sets.size() != 1) return false;
    if (dst.uv_sets[0].purpose != UVSetPurpose::Texture) return false;
    if (dst.uv_sets[0].coords.size() != 4) return false;
    if (dst.material_idx != 7) return false;
    return true;
}

bool test_to_processable_uses_raw_indices_not_packed() {
    // mesh.indices (the post-pack buffer) must NOT leak into IR. IR.indices
    // is sourced from raw_indices.
    mesh src;
    for (u32 i = 0; i < 3; ++i) src.positions.emplace_back(v3{0.f, 0.f, 0.f});
    src.raw_indices.emplace_back(0);
    src.raw_indices.emplace_back(1);
    src.raw_indices.emplace_back(2);
    src.indices.emplace_back(9);   // post-pack garbage, must be ignored
    src.indices.emplace_back(9);
    src.indices.emplace_back(9);
    ProcessableMesh dst;
    to_processable(src, dst);
    return vec_eq_u32(dst.indices, src.raw_indices);
}

bool test_to_processable_default_uv_purpose() {
    // Legacy uv_sets has no purpose tag; every channel defaults to Texture.
    mesh src;
    for (u32 i = 0; i < 3; ++i) src.positions.emplace_back(v3{0.f, 0.f, 0.f});
    src.raw_indices.emplace_back(0);
    src.raw_indices.emplace_back(1);
    src.raw_indices.emplace_back(2);
    src.uv_sets.resize(2);
    for (u32 i = 0; i < 3; ++i) src.uv_sets[0].emplace_back(v2{0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) src.uv_sets[1].emplace_back(v2{1.f, 1.f});
    ProcessableMesh dst;
    to_processable(src, dst);
    if (dst.uv_sets.size() != 2) return false;
    for (const auto& s : dst.uv_sets) {
        if (s.purpose != UVSetPurpose::Texture) return false;
    }
    return true;
}

bool test_round_trip_preserves_ir() {
    // legacy → IR → legacy must preserve every IR-carrying field.
    mesh original = make_legacy_fixture();
    ProcessableMesh ir;
    to_processable(original, ir);
    mesh reconstructed;
    from_processable(ir, reconstructed);

    if (reconstructed.name != original.name) return false;
    if (!vec_eq_v3(reconstructed.positions,  original.positions))  return false;
    if (!vec_eq_u32(reconstructed.raw_indices, original.raw_indices)) return false;
    if (!vec_eq_v3(reconstructed.normals,   original.normals))   return false;
    if (!vec_eq_v4(reconstructed.tangents,  original.tangents))  return false;
    if (!vec_eq_v3(reconstructed.colors,    original.colors))    return false;
    if (reconstructed.material_idx != original.material_idx) return false;
    if (reconstructed.uv_sets.size() != original.uv_sets.size()) return false;
    for (u32 i = 0; i < (u32)reconstructed.uv_sets.size(); ++i) {
        if (!vec_eq_v2(reconstructed.uv_sets[i], original.uv_sets[i])) return false;
    }
    return true;
}

bool test_from_processable_clears_packing_artifacts() {
    // dst comes in with stale packing artifacts; from_processable must reset
    // them so a fresh pack_mesh_data run starts clean.
    mesh dst;
    for (u32 i = 0; i < 8; ++i) dst.vertices.emplace_back();
    for (u32 i = 0; i < 3; ++i) dst.indices.emplace_back(0);
    for (u32 i = 0; i < 64; ++i) dst.position_buffer.emplace_back(0xAA);
    for (u32 i = 0; i < 32; ++i) dst.element_buffer.emplace_back(0xBB);
    for (u32 i = 0; i < 2; ++i)  dst.meshlets.emplace_back();
    for (u32 i = 0; i < 16; ++i) dst.sdf.data.emplace_back(0x1234);
    dst.lod_threshold = 0.5f;
    dst.lod_id        = 3;

    ProcessableMesh src;
    for (u32 i = 0; i < 3; ++i) src.positions.emplace_back(v3{0.f, 0.f, 0.f});
    for (u32 i = 0; i < 3; ++i) src.indices.emplace_back(i);
    from_processable(src, dst);

    if (!dst.vertices.empty())          return false;
    if (!dst.indices.empty())           return false;
    if (!dst.position_buffer.empty())   return false;
    if (!dst.element_buffer.empty())    return false;
    if (!dst.meshlets.empty())          return false;
    if (!dst.sdf.data.empty())          return false;
    if (dst.lod_threshold != -1.f)      return false;
    if (dst.lod_id        != u32_invalid_id) return false;
    return true;
}

// --- Test runner ----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };

#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_validate_ok_complete),
        CASE(test_validate_ok_minimal),
        CASE(test_validate_fail_empty_positions),
        CASE(test_validate_fail_index_count),
        CASE(test_validate_fail_normal_count),
        CASE(test_validate_fail_uv_count),
        CASE(test_validate_uv_empty_ok),
        CASE(test_to_processable_copies_ir_fields),
        CASE(test_to_processable_uses_raw_indices_not_packed),
        CASE(test_to_processable_default_uv_purpose),
        CASE(test_round_trip_preserves_ir),
        CASE(test_from_processable_clears_packing_artifacts),
    };

    int passed = 0;
    int failed = 0;
    for (const auto& c : cases) {
        bool ok = false;
        try { ok = c.fn(); }
        catch (...) { ok = false; }
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << c.name << "\n";
        if (ok) ++passed; else ++failed;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
