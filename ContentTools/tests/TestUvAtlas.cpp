// M6 unit tests for the uvatlas module.
//
// Verifies the xatlas integration against synthetic fixtures:
//   - Texture UV generation on a cube — runs, output UVs in [0,1].
//   - Lightmap UV with strict_unique_pack — UVs strictly in atlas bounds.
//   - Multi-UVSet preservation — both pre-existing channels are carried
//     through xatlas's vertex re-permutation via the xref map.
//   - Aux attribute preservation — normals carried through xref.
//   - Empty mesh short-circuits with a warning.
//   - Triangle count preserved for a closed mesh (xatlas shouldn't drop).

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "modules/uvatlas/UvAtlas.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::uvatlas;

namespace {

// Closed axis-aligned cube, 8 verts + 12 tris. Cleanest manifold fixture
// for unwrapping: xatlas should produce 6 charts (one per face).
ProcessableMesh make_cube() {
    ProcessableMesh m;
    m.positions.emplace_back(v3{ 0.f,  0.f,  0.f});   // 0
    m.positions.emplace_back(v3{ 1.f,  0.f,  0.f});   // 1
    m.positions.emplace_back(v3{ 1.f,  1.f,  0.f});   // 2
    m.positions.emplace_back(v3{ 0.f,  1.f,  0.f});   // 3
    m.positions.emplace_back(v3{ 0.f,  0.f,  1.f});   // 4
    m.positions.emplace_back(v3{ 1.f,  0.f,  1.f});   // 5
    m.positions.emplace_back(v3{ 1.f,  1.f,  1.f});   // 6
    m.positions.emplace_back(v3{ 0.f,  1.f,  1.f});   // 7
    // 6 faces × 2 tris, outward winding.
    static const u32 tris[12][3] = {
        {0, 2, 1}, {0, 3, 2},   // -Z
        {4, 5, 6}, {4, 6, 7},   // +Z
        {0, 1, 5}, {0, 5, 4},   // -Y
        {3, 6, 2}, {3, 7, 6},   // +Y
        {0, 4, 7}, {0, 7, 3},   // -X
        {1, 2, 6}, {1, 6, 5},   // +X
    };
    for (const auto& t : tris) {
        m.indices.emplace_back(t[0]);
        m.indices.emplace_back(t[1]);
        m.indices.emplace_back(t[2]);
    }
    return m;
}

bool in_unit_range(const v2& uv) {
    return uv.x >= -1e-4f && uv.x <= 1.f + 1e-4f &&
           uv.y >= -1e-4f && uv.y <= 1.f + 1e-4f;
}

bool finite_vec(const v2& uv) {
    return std::isfinite(uv.x) && std::isfinite(uv.y);
}

}  // namespace

// --- Texture UV -----------------------------------------------------------

bool test_texture_uv_cube_runs_and_normalizes() {
    ProcessableMesh m = make_cube();
    const u32 tris_before = (u32)m.indices.size() / 3;
    Params p = ForTexture();
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;

    const UVSet* tex = find_uv_set(m, UVSetPurpose::Texture);
    if (!tex) return false;
    if (tex->coords.size() != m.positions.size()) return false;
    for (const auto& uv : tex->coords) {
        if (!finite_vec(uv)) return false;
        if (!in_unit_range(uv)) return false;
    }
    // Triangle count must be preserved for a clean closed mesh.
    if ((u32)m.indices.size() / 3 != tris_before) return false;
    return true;
}

// --- Lightmap UV ----------------------------------------------------------

bool test_lightmap_uv_cube_strict_in_range() {
    ProcessableMesh m = make_cube();
    Params p = ForLightmap();
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;

    const UVSet* lm = find_uv_set(m, UVSetPurpose::Lightmap);
    if (!lm) return false;
    if (lm->coords.size() != m.positions.size()) return false;
    // Strict unique pack: every UV must lie inside [0,1].
    for (const auto& uv : lm->coords) {
        if (!finite_vec(uv)) return false;
        if (!in_unit_range(uv)) return false;
    }
    return true;
}

// --- Multi-UVSet preservation --------------------------------------------

bool test_multi_uvset_preservation_through_xref() {
    ProcessableMesh m = make_cube();

    // Pre-populate two UVSets:
    //   - Texture-purpose: filled with a sentinel pattern so we can verify
    //     the values survive the xref permutation.
    //   - Lightmap-purpose: another sentinel pattern.
    UVSet* tex_in = find_uv_set(m, UVSetPurpose::Texture);
    if (!tex_in) {
        m.uv_sets.emplace_back();
        m.uv_sets.back().purpose = UVSetPurpose::Texture;
        tex_in = &m.uv_sets.back();
    }
    tex_in->coords.clear();
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        tex_in->coords.emplace_back(v2{(f32)i * 0.1f, (f32)i * 0.2f});
    }
    m.uv_sets.emplace_back();
    m.uv_sets.back().purpose = UVSetPurpose::Lightmap;
    UVSet* lm_in = &m.uv_sets.back();
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        lm_in->coords.emplace_back(v2{(f32)i * 0.3f, (f32)i * 0.4f});
    }

    Params p = ForTexture();
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;

    // After Run: the Lightmap UVSet (not the target) must still be present,
    // sized == new vertex count, and contain values pulled from the original
    // sentinel pattern (modulo xref permutation, so values must be a subset
    // of the original sentinel set).
    const UVSet* lm_out = find_uv_set(m, UVSetPurpose::Lightmap);
    if (!lm_out) return false;
    if (lm_out->coords.size() != m.positions.size()) return false;
    // Every coord must match SOME original sentinel (since xref maps output
    // vertices back to inputs, only original values should appear).
    for (const auto& uv : lm_out->coords) {
        bool match = false;
        for (u32 i = 0; i < 8; ++i) {
            const v2 sent{(f32)i * 0.3f, (f32)i * 0.4f};
            if (std::fabs(uv.x - sent.x) < 1e-5f &&
                std::fabs(uv.y - sent.y) < 1e-5f) { match = true; break; }
        }
        if (!match) return false;
    }
    return true;
}

// --- Aux attribute preservation ------------------------------------------

bool test_normals_preserved_through_xref() {
    ProcessableMesh m = make_cube();
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        // Sentinel normals — one distinct value per input vertex.
        m.normals.emplace_back(v3{(f32)i, (f32)i + 0.5f, (f32)i + 0.25f});
    }
    Params p = ForTexture();
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    if (m.normals.size() != m.positions.size()) return false;
    // Every output normal must be one of the original sentinels.
    for (const auto& n : m.normals) {
        bool match = false;
        for (u32 i = 0; i < 8; ++i) {
            const v3 sent{(f32)i, (f32)i + 0.5f, (f32)i + 0.25f};
            if (std::fabs(n.x - sent.x) < 1e-5f &&
                std::fabs(n.y - sent.y) < 1e-5f &&
                std::fabs(n.z - sent.z) < 1e-5f) { match = true; break; }
        }
        if (!match) return false;
    }
    return true;
}

// --- Edge cases -----------------------------------------------------------

bool test_empty_mesh_returns_warning() {
    ProcessableMesh m;   // empty
    Params p;
    primal::utl::vector<ErrorReport> errs;
    bool ok = Run(m, p, errs);
    if (ok) return false;
    bool found = false;
    for (const auto& e : errs) {
        if (e.code == "uvatlas.empty_input") { found = true; break; }
    }
    return found;
}

bool test_triangle_count_preserved_for_closed_mesh() {
    ProcessableMesh m = make_cube();
    const u32 tris_before = (u32)m.indices.size() / 3;
    Params p = ForTexture();
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    const u32 tris_after = (u32)m.indices.size() / 3;
    // For a closed, clean manifold xatlas should not drop triangles.
    return tris_after == tris_before;
}

// --- Test runner ----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_texture_uv_cube_runs_and_normalizes),
        CASE(test_lightmap_uv_cube_strict_in_range),
        CASE(test_multi_uvset_preservation_through_xref),
        CASE(test_normals_preserved_through_xref),
        CASE(test_empty_mesh_returns_warning),
        CASE(test_triangle_count_preserved_for_closed_mesh),
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
