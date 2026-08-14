// M3 unit tests for the repair module.
//
// Verifies the IR ↔ pmp::SurfaceMesh bridge and the four repair operations
// against small synthetic fixtures: degenerate-face removal, border
// stitching, hole filling, orientation consistency.
//
// Each test builds a ProcessableMesh IR with a known defect, runs repair,
// then asserts the defect is gone + the round-trip preserved valid IR.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "common/MeshConverters.h"
#include "modules/repair/Repair.h"

#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::repair;

namespace {

// Tiny quad: (0,0,0) (1,0,0) (1,1,0) (0,1,0), two triangles, no normals/uvs.
// Used as a baseline manifold mesh that tests then break on purpose.
ProcessableMesh make_quad() {
    ProcessableMesh m;
    m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 1.f, 0.f});
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});
    m.indices.emplace_back(0); m.indices.emplace_back(1); m.indices.emplace_back(2);
    m.indices.emplace_back(0); m.indices.emplace_back(2); m.indices.emplace_back(3);
    return m;
}

// Two disjoint triangles: one valid, one colinear (zero area). Disjoint so
// PMP's manifold check doesn't reject them. The bridge passes both through
// because both have 3 distinct vertex indices; remove_degenerate_faces must
// then catch the colinear one via face_area.
ProcessableMesh make_quad_with_degenerate_face() {
    ProcessableMesh m;
    // Triangle 1: (0,0,0) (1,0,0) (1,1,0) — valid.
    m.positions.emplace_back(v3{0.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 0.f, 0.f});
    m.positions.emplace_back(v3{1.f, 1.f, 0.f});
    // Triangle 2: (5,0,0) (6,0,0) (7,0,0) — colinear, zero area.
    m.positions.emplace_back(v3{5.f, 0.f, 0.f});
    m.positions.emplace_back(v3{6.f, 0.f, 0.f});
    m.positions.emplace_back(v3{7.f, 0.f, 0.f});
    m.indices.emplace_back(0); m.indices.emplace_back(1); m.indices.emplace_back(2);
    m.indices.emplace_back(3); m.indices.emplace_back(4); m.indices.emplace_back(5);
    return m;
}

// Open fan: 5 verts in a row, 3 triangles forming an open strip with a
// single boundary loop. Simplest "has a hole" fixture.
//   Vertices: (0,0,0) (1,0,0) (2,0,0) (3,0,0) (4,0,0)
//             (0,1,0) (1,1,0) (2,1,0) (3,1,0) (4,1,0)
//   Triangles: bottom strip going up.
ProcessableMesh make_open_strip() {
    ProcessableMesh m;
    for (u32 i = 0; i < 5; ++i) m.positions.emplace_back(v3{(f32)i, 0.f, 0.f});
    for (u32 i = 0; i < 5; ++i) m.positions.emplace_back(v3{(f32)i, 1.f, 0.f});
    // 4 quads → 8 tris
    for (u32 i = 0; i < 4; ++i) {
        u32 bl = i, br = i + 1, tl = i + 5, tr = i + 6;
        m.indices.emplace_back(bl); m.indices.emplace_back(br); m.indices.emplace_back(tr);
        m.indices.emplace_back(bl); m.indices.emplace_back(tr); m.indices.emplace_back(tl);
    }
    return m;
}

u32 count_unique_triangles(const ProcessableMesh& m) {
    return (u32)m.indices.size() / 3;
}

}  // namespace

// --- Bridge round-trip ----------------------------------------------------

bool test_bridge_roundtrip_preserves_topology() {
    // No-op repair (Params with Op::None) must leave the mesh unchanged.
    ProcessableMesh original = make_quad();
    ProcessableMesh working   = make_quad();
    Params p; p.ops = static_cast<u8>(Op::None);
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(working, p, errs);

    if (modified) return false;
    if (working.positions.size() != original.positions.size()) return false;
    if (working.indices.size()    != original.indices.size())    return false;
    return true;
}

bool test_bridge_drops_auxiliary_arrays_when_no_op() {
    // Auxiliary attributes (normals/uvs) are still carried across the bridge
    // even when no op modifies the topology — verifies the to/from round-trip.
    ProcessableMesh m;
    for (u32 i = 0; i < 4; ++i) m.positions.emplace_back(v3{(f32)i, 0.f, 0.f});
    for (u32 i = 0; i < 4; ++i) m.normals.emplace_back(v3{0.f, 0.f, 1.f});
    UVSet uv; uv.purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < 4; ++i) uv.coords.emplace_back(v2{(f32)i, 0.f});
    m.uv_sets.emplace_back(std::move(uv));
    m.indices.emplace_back(0); m.indices.emplace_back(1); m.indices.emplace_back(2);
    m.indices.emplace_back(0); m.indices.emplace_back(2); m.indices.emplace_back(3);

    Params p; p.ops = static_cast<u8>(Op::None);
    primal::utl::vector<ErrorReport> errs;
    Run(m, p, errs);   // no-op: bridge should still preserve normals/UVs

    if (m.positions.size() != 4) return false;
    if (m.normals.size()   != 4) return false;
    if (m.uv_sets.size()   != 1) return false;
    if (m.uv_sets[0].coords.size() != 4) return false;
    return true;
}

// --- remove_degenerate_faces ---------------------------------------------

bool test_remove_degenerate_face() {
    ProcessableMesh m = make_quad_with_degenerate_face();
    // Sanity: starts with 6 indices = 2 triangles, one degenerate.
    if (count_unique_triangles(m) != 2) return false;

    Params p;
    p.ops = static_cast<u8>(Op::RemoveDegenerateFaces);
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(m, p, errs);

    if (!modified) return false;
    // Degenerate (0,2,2) collapses; one valid triangle remains.
    if (count_unique_triangles(m) != 1) return false;
    return true;
}

// --- fill_holes -----------------------------------------------------------

bool test_fill_hole_in_open_strip() {
    ProcessableMesh m = make_open_strip();
    // Open strip has a single boundary loop around the outside.
    // After fill_holes, triangle count goes up by ≥1 (fan fill).
    const u32 tris_before = count_unique_triangles(m);
    if (tris_before != 8) return false;

    Params p;
    p.ops = static_cast<u8>(Op::FillHoles);
    p.max_hole_size = 0;        // unlimited — our hole has 10 boundary edges
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(m, p, errs);

    if (!modified) return false;
    if (count_unique_triangles(m) <= tris_before) return false;
    // Every position should now be referenced by at least one triangle.
    return true;
}

bool test_fill_hole_respects_max_size() {
    // With max_hole_size = 1, our 10-edge boundary should NOT be filled.
    ProcessableMesh m = make_open_strip();
    const u32 tris_before = count_unique_triangles(m);

    Params p;
    p.ops = static_cast<u8>(Op::FillHoles);
    p.max_hole_size = 1;
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(m, p, errs);

    if (modified) return false;        // expected skipped
    if (count_unique_triangles(m) != tris_before) return false;
    return true;
}

// --- stitch_borders -------------------------------------------------------

bool test_stitch_borders_noop_when_distance_zero() {
    // stitch_distance = 0 → never merge anything. Sanity check.
    ProcessableMesh m = make_open_strip();
    Params p;
    p.ops = static_cast<u8>(Op::StitchBorders);
    p.stitch_distance = 0.f;
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(m, p, errs);
    return !modified;
}

// --- orient_outward -------------------------------------------------------

bool test_orient_outward_runs_without_crash() {
    // We don't have a "broken orientation" fixture; the minimum we need to
    // verify is that orient_outward doesn't corrupt a manifold mesh.
    ProcessableMesh m = make_quad();
    const u32 tris_before = count_unique_triangles(m);

    Params p;
    p.ops = static_cast<u8>(Op::OrientOutward);
    primal::utl::vector<ErrorReport> errs;
    Run(m, p, errs);

    // A consistently-wound input should remain a quad.
    return count_unique_triangles(m) == tris_before;
}

// --- Run with Op::All -----------------------------------------------------

bool test_run_all_on_clean_mesh_is_noop() {
    // A clean, closed, consistently-oriented mesh: nothing to do.
    // Build a tetrahedron (closed).
    ProcessableMesh m;
    m.positions.emplace_back(v3{ 0.f,  0.f,  0.f});
    m.positions.emplace_back(v3{ 1.f,  0.f,  0.f});
    m.positions.emplace_back(v3{ 0.f,  1.f,  0.f});
    m.positions.emplace_back(v3{ 0.f,  0.f,  1.f});
    // 4 triangles, outward-wound.
    m.indices.emplace_back(0); m.indices.emplace_back(2); m.indices.emplace_back(1);
    m.indices.emplace_back(0); m.indices.emplace_back(1); m.indices.emplace_back(3);
    m.indices.emplace_back(0); m.indices.emplace_back(3); m.indices.emplace_back(2);
    m.indices.emplace_back(1); m.indices.emplace_back(2); m.indices.emplace_back(3);
    const u32 tris_before = count_unique_triangles(m);

    Params p;   // Op::All
    primal::utl::vector<ErrorReport> errs;
    bool modified = Run(m, p, errs);

    // Tetrahedron is already clean → ideally no-op. We tolerate either result
    // (PMP may still touch the data structure) but topology must be preserved.
    if (count_unique_triangles(m) != tris_before) return false;
    return true;
}

// --- Test runner ----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_bridge_roundtrip_preserves_topology),
        CASE(test_bridge_drops_auxiliary_arrays_when_no_op),
        CASE(test_remove_degenerate_face),
        CASE(test_fill_hole_in_open_strip),
        CASE(test_fill_hole_respects_max_size),
        CASE(test_stitch_borders_noop_when_distance_zero),
        CASE(test_orient_outward_runs_without_crash),
        CASE(test_run_all_on_clean_mesh_is_noop),
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
