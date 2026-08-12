// M5 unit tests for the subdivide module.
//
// Verifies the two PMP subdivision schemes:
//   - Loop          on a tetrahedron → 4^N × faces per level
//   - CatmullClark  on a tetrahedron → at least 4× faces (CC produces quads
//                   from triangles; we only check it runs and grows).
//
// Plus an edge case: empty mesh short-circuits with a warning, levels=0 is
// a no-op.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "modules/subdivide/Subdivide.h"

#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::subdivide;

namespace {

// Tetrahedron — 4 verts, 4 triangles, closed manifold. Smallest valid
// triangle mesh for subdivision tests.
ProcessableMesh make_tetrahedron() {
    ProcessableMesh m;
    m.positions.emplace_back(v3{ 1.f,  1.f,  1.f});
    m.positions.emplace_back(v3{-1.f, -1.f,  1.f});
    m.positions.emplace_back(v3{-1.f,  1.f, -1.f});
    m.positions.emplace_back(v3{ 1.f, -1.f, -1.f});
    // Outward-facing winding.
    static const u32 tris[4][3] = {
        {0, 1, 2},
        {0, 3, 1},
        {0, 2, 3},
        {1, 3, 2},
    };
    for (const auto& t : tris) {
        m.indices.emplace_back(t[0]);
        m.indices.emplace_back(t[1]);
        m.indices.emplace_back(t[2]);
    }
    return m;
}

u32 face_count(const ProcessableMesh& m) { return (u32)m.indices.size() / 3; }

}  // namespace

// --- Loop -----------------------------------------------------------------

bool test_loop_single_level_quadruples_faces() {
    ProcessableMesh m = make_tetrahedron();
    const u32 faces_before = face_count(m);   // 4

    Params p;
    p.scheme = Scheme::Loop;
    p.levels = 1;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    // Loop on a triangle mesh: each face → 4 faces, so 4 → 16.
    return face_count(m) == faces_before * 4;
}

bool test_loop_two_levels_multiply_16x() {
    ProcessableMesh m = make_tetrahedron();
    const u32 faces_before = face_count(m);

    Params p;
    p.scheme = Scheme::Loop;
    p.levels = 2;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    // 4^2 = 16, so 4 → 64.
    return face_count(m) == faces_before * 16;
}

bool test_loop_preserves_aux_attributes_length() {
    // Aux attributes (normals here) are carried as VertexProperty through
    // the PMP run; new vertices get default-zero. The IR after subdivision
    // should have normals.size() == positions.size() (one per vertex).
    ProcessableMesh m = make_tetrahedron();
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        m.normals.emplace_back(v3{0.f, 0.f, 1.f});
    }
    Params p;
    p.scheme = Scheme::Loop;
    p.levels = 1;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    if (m.normals.size() != m.positions.size()) return false;
    return true;
}

// --- Catmull-Clark --------------------------------------------------------

bool test_catmull_clark_runs_and_grows_face_count() {
    ProcessableMesh m = make_tetrahedron();
    const u32 faces_before = face_count(m);

    Params p;
    p.scheme = Scheme::CatmullClark;
    p.levels = 1;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    // CC on a triangle mesh produces a quad-dominant output; face count
    // grows but not by exactly 4×. Just check it grew.
    return face_count(m) > faces_before;
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
        if (e.code == "subdivide.empty_input") { found = true; break; }
    }
    return found;
}

bool test_levels_zero_is_noop() {
    ProcessableMesh m = make_tetrahedron();
    const u32 faces_before = face_count(m);
    Params p;
    p.levels = 0;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    return face_count(m) == faces_before;
}

// Two tetrahedra sharing a single edge — both cap triangles share that edge,
// making it non-manifold (4 incident faces). Pre-M13 this crashed with
// pmp::TopologyException. Post-fix: subdivide drops the offending faces,
// emits a "subdivide.dropped_non_manifold" warning, and subdivides the
// remaining manifold portion.
bool test_non_manifold_input_drops_instead_of_crashing() {
    ProcessableMesh m;
    m.positions.emplace_back(v3{ 0.f, 0.f, 0.f});  // 0
    m.positions.emplace_back(v3{ 1.f, 0.f, 0.f});  // 1  (shared edge 0-1)
    m.positions.emplace_back(v3{ 0.f, 1.f, 0.f});  // 2  (tet A cap)
    m.positions.emplace_back(v3{ 0.f, 0.f, 1.f});  // 3  (tet A apex)
    m.positions.emplace_back(v3{ 0.f,-1.f, 0.f});  // 4  (tet B cap)
    m.positions.emplace_back(v3{ 0.f, 0.f,-1.f});  // 5  (tet B apex)
    // Tet A: edge 0-1 has 2 incident faces (0,1,2)(0,3,1)
    static const u32 tet_a[] = {0,1,2, 0,3,1};
    for (auto i : tet_a) m.indices.emplace_back(i);
    // Tet B: edge 0-1 gets 2 more incident faces (0,4,1)(0,1,5) → non-manifold
    static const u32 tet_b[] = {0,4,1, 0,1,5};
    for (auto i : tet_b) m.indices.emplace_back(i);

    const u32 faces_before = face_count(m);   // 4
    Params p;
    p.scheme = Scheme::Loop;
    p.levels = 1;
    primal::utl::vector<ErrorReport> errs;
    const bool ok = Run(m, p, errs);
    if (!ok) return false;

    bool found_warning = false;
    for (const auto& e : errs) {
        if (e.code == "subdivide.dropped_non_manifold") {
            found_warning = true;
            break;
        }
    }
    if (!found_warning) return false;
    // Result should have grown from the manifold portion (>= 2 faces survived
    // × 4 from Loop = >= 8). Exact count depends on which faces were dropped.
    return face_count(m) >= 8 && face_count(m) < faces_before * 4;
}

// --- Test runner ----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_loop_single_level_quadruples_faces),
        CASE(test_loop_two_levels_multiply_16x),
        CASE(test_loop_preserves_aux_attributes_length),
        CASE(test_catmull_clark_runs_and_grows_face_count),
        CASE(test_empty_mesh_returns_warning),
        CASE(test_levels_zero_is_noop),
        CASE(test_non_manifold_input_drops_instead_of_crashing),
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
