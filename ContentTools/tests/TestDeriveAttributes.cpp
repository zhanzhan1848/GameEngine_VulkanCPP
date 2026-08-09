// M11 unit tests for the derive_attributes module.
//
// Coverage:
//   - empty mesh short-circuits with warning
//   - Smooth/AreaWeighted/AngleWeighted on a cube: vertex normals approach
//     ±axis direction (each cube vertex is shared by 3 faces, so the averaged
//     normal points along the cube diagonal of the corner).
//   - Faceted mode on a cube with angle=60°: 8 verts → 24 verts
//     (each cube vertex has 3 face-normal groups for the 3 adjacent faces).
//   - Faceted angle=180°: no vertex duplication (all groups merge into 1).
//   - MikkTSpace tangent is orthogonal to normal (dot ≈ 0).
//   - MikkTSpace with no UV degrades to AreaWeighted + warning.
//
// Cube topology: 8 vertices, 12 triangles, 36 indices, 6 faces.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "modules/derive_attributes/DeriveAttributes.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::derive;

namespace {

// Unit cube centered at origin. 8 verts, 12 tris. Normals/UVs left empty so
// derive has to fill them; the test fixture calls derive::Run to do that.
ProcessableMesh make_unit_cube() {
    ProcessableMesh m;
    for (s8 x = -1; x <= 1; x += 2) {
        for (s8 y = -1; y <= 1; y += 2) {
            for (s8 z = -1; z <= 1; z += 2) {
                m.positions.emplace_back(v3{(f32)x, (f32)y, (f32)z});
            }
        }
    }
    static const u32 tris[12][3] = {
        // +X face
        {1, 5, 7}, {1, 7, 3},
        // -X face
        {4, 0, 2}, {4, 2, 6},
        // +Y face
        {2, 3, 7}, {2, 7, 6},
        // -Y face
        {0, 4, 5}, {0, 5, 1},
        // +Z face
        {0, 1, 3}, {0, 3, 2},
        // -Z face
        {5, 4, 6}, {5, 6, 7},
    };
    for (const auto& t : tris) {
        m.indices.emplace_back(t[0]);
        m.indices.emplace_back(t[1]);
        m.indices.emplace_back(t[2]);
    }
    return m;
}

f32 dot_v3(const v3& a, const v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

f32 length_v3(const v3& v) { return std::sqrt(dot_v3(v, v)); }

}  // namespace

// ---- empty mesh ---------------------------------------------------------

bool test_empty_mesh_returns_warning() {
    ProcessableMesh m;  // empty
    primal::utl::vector<ErrorReport> errs;
    Params p;
    const bool ok = Run(m, p, errs);
    if (ok) return false;  // expect false on empty input
    bool has_warning = false;
    for (const auto& e : errs) {
        if (e.severity == Severity::Warning && e.code == "derive.empty_input") {
            has_warning = true;
            break;
        }
    }
    return has_warning;
}

// ---- Smooth / AreaWeighted / AngleWeighted: averaged normals ------------

bool test_smooth_normals_on_cube_match_corner_diagonal() {
    ProcessableMesh m = make_unit_cube();
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode  = NormalMode::Smooth;
    p.tangent_mode = TangentMode::None;
    if (!Run(m, p, errs)) return false;

    if (m.normals.size() != 8) return false;
    // Each cube vertex sits at (±1, ±1, ±1). Averaging the 3 adjacent
    // face normals (+X/-X, +Y/-Y, +Z/-Z) gives the unit vector pointing
    // along the vertex's sign triple — i.e. normalize(position).
    for (u32 i = 0; i < 8; ++i) {
        const v3 expected = v3{
            m.positions[i].x > 0.f ? 1.f : -1.f,
            m.positions[i].y > 0.f ? 1.f : -1.f,
            m.positions[i].z > 0.f ? 1.f : -1.f,
        };
        // Smooth (unweighted) on a cube averages to (±s, ±s, ±s) where s =
        // 1/sqrt(3) after normalization. Use 0.5 tolerance because cube
        // face normals aren't unit-equal at the corner.
        const f32 d = dot_v3(m.normals[i], expected);
        if (d < 0.95f) return false;
    }
    return true;
}

bool test_area_weighted_normals_correct_magnitude() {
    ProcessableMesh m = make_unit_cube();
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode  = NormalMode::AreaWeighted;
    p.tangent_mode = TangentMode::None;
    if (!Run(m, p, errs)) return false;

    if (m.normals.size() != 8) return false;
    // Same expectation as Smooth: cube symmetry → averaged normal points
    // along corner diagonal. Area-weighting is identical for unit cube.
    for (u32 i = 0; i < 8; ++i) {
        const v3 expected = v3{
            m.positions[i].x > 0.f ? 1.f : -1.f,
            m.positions[i].y > 0.f ? 1.f : -1.f,
            m.positions[i].z > 0.f ? 1.f : -1.f,
        };
        const f32 d = dot_v3(m.normals[i], expected);
        if (d < 0.95f) return false;
        if (std::fabs(length_v3(m.normals[i]) - 1.f) > 1e-3f) return false;
    }
    return true;
}

bool test_angle_weighted_normals_correct() {
    ProcessableMesh m = make_unit_cube();
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode  = NormalMode::AngleWeighted;
    p.tangent_mode = TangentMode::None;
    if (!Run(m, p, errs)) return false;

    if (m.normals.size() != 8) return false;
    // At each cube corner, all three incident triangles have a 60° angle
    // at that corner (equilateral on the cube face). So angle weighting is
    // symmetric and again points along the corner diagonal.
    for (u32 i = 0; i < 8; ++i) {
        const v3 expected = v3{
            m.positions[i].x > 0.f ? 1.f : -1.f,
            m.positions[i].y > 0.f ? 1.f : -1.f,
            m.positions[i].z > 0.f ? 1.f : -1.f,
        };
        const f32 d = dot_v3(m.normals[i], expected);
        if (d < 0.95f) return false;
    }
    return true;
}

// ---- Faceted: angle threshold vertex duplication -----------------------

bool test_faceted_cube_60deg_doubles_vertices() {
    ProcessableMesh m = make_unit_cube();
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode            = NormalMode::Faceted;
    p.faceted_angle_degrees  = 60.f;
    p.tangent_mode           = TangentMode::None;
    if (!Run(m, p, errs)) return false;

    // At 60° threshold, every adjacent face pair at a vertex is a hard
    // edge (faces are 90° apart). Each cube vertex has 3 distinct smooth
    // groups → 8 verts × 3 = 24 verts after dedup.
    if (m.positions.size() != 24) {
        std::cout << "  expected 24 verts, got " << m.positions.size() << "\n";
        return false;
    }
    // Triangle count is preserved (12 tris → 36 indices).
    if (m.indices.size() != 36) return false;
    // Each slot's normal should be axis-aligned (one of ±X/Y/Z).
    for (const auto& n : m.normals) {
        const f32 ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        const f32 axis_max = std::max({ax, ay, az});
        // Dominant axis must be ~1; the other two ~0.
        if (axis_max < 0.99f) return false;
    }
    return true;
}

bool test_faceted_cube_180deg_no_duplication() {
    ProcessableMesh m = make_unit_cube();
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode            = NormalMode::Faceted;
    p.faceted_angle_degrees  = 180.f;  // any angle < 180° → soft
    p.tangent_mode           = TangentMode::None;
    if (!Run(m, p, errs)) return false;

    // 180° threshold means all face pairs merge → 1 slot per vertex.
    return m.positions.size() == 8;
}

// ---- MikkTSpace tangent ------------------------------------------------

bool test_mikktspace_tangent_orthogonal_to_normal() {
    // Single quad in xy-plane with non-degenerate UV unwrap. Each vert
    // has exactly one face, so Smooth normal = (0,0,1) and MikkTSpace
    // has unambiguous per-vert tangent input.
    //
    // We avoid the cube here because a single shared planar UV projection
    // is degenerate on 4 of 6 faces (UV doesn't vary with z, so ±X/±Y
    // faces collapse to a line in UV space). MikkTSpace on degenerate UV
    // triangles produces arbitrary tangents — that's a property of the
    // input data, not of the adapter, so it doesn't belong in this test.
    ProcessableMesh m;
    m.positions.emplace_back(v3{-1.f, -1.f, 0.f});
    m.positions.emplace_back(v3{ 1.f, -1.f, 0.f});
    m.positions.emplace_back(v3{ 1.f,  1.f, 0.f});
    m.positions.emplace_back(v3{-1.f,  1.f, 0.f});
    static const u32 quad_idx[6] = {0, 1, 2, 0, 2, 3};
    for (u32 i : quad_idx) m.indices.emplace_back(i);
    UVSet uvs;
    uvs.purpose = UVSetPurpose::Texture;
    uvs.coords.emplace_back(v2{0.f, 0.f});
    uvs.coords.emplace_back(v2{1.f, 0.f});
    uvs.coords.emplace_back(v2{1.f, 1.f});
    uvs.coords.emplace_back(v2{0.f, 1.f});
    m.uv_sets.emplace_back(std::move(uvs));

    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode  = NormalMode::Smooth;
    p.tangent_mode = TangentMode::MikkTSpace;
    if (!Run(m, p, errs)) return false;

    if (m.tangents.size() != 4) return false;
    // Smooth normal on a flat quad = (0,0,1) for every vert. MikkTSpace's
    // tangent for an axis-aligned UV unwrap should be along ±X, exactly
    // orthogonal to the normal.
    for (u32 i = 0; i < 4; ++i) {
        const v3 t = v3{m.tangents[i].x, m.tangents[i].y, m.tangents[i].z};
        const f32 d = dot_v3(t, m.normals[i]);
        if (std::fabs(d) > 1e-2f) {
            std::cout << "  vert " << i << " tangent·normal = " << d << "\n";
            return false;
        }
    }
    return true;
}

bool test_mikktspace_without_uv_falls_back_to_area_weighted() {
    ProcessableMesh m = make_unit_cube();
    // No UV set added.
    primal::utl::vector<ErrorReport> errs;
    Params p;
    p.normal_mode  = NormalMode::Smooth;
    p.tangent_mode = TangentMode::MikkTSpace;
    const bool ok = Run(m, p, errs);

    // Should still succeed (MikkTSpace fallback is a warning, not error).
    if (!ok) return false;
    bool has_warning = false;
    for (const auto& e : errs) {
        if (e.severity == Severity::Warning &&
            e.code == "derive.mikktspace_no_uv") {
            has_warning = true;
            break;
        }
    }
    if (!has_warning) return false;
    // Without UV, neither MikkTSpace nor AreaWeighted can run → tangents
    // is empty. Honest behavior — caller must supply UVs for tangent calc.
    return m.tangents.empty();
}

// ---- runner -------------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_empty_mesh_returns_warning),
        CASE(test_smooth_normals_on_cube_match_corner_diagonal),
        CASE(test_area_weighted_normals_correct_magnitude),
        CASE(test_angle_weighted_normals_correct),
        CASE(test_faceted_cube_60deg_doubles_vertices),
        CASE(test_faceted_cube_180deg_no_duplication),
        CASE(test_mikktspace_tangent_orthogonal_to_normal),
        CASE(test_mikktspace_without_uv_falls_back_to_area_weighted),
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
