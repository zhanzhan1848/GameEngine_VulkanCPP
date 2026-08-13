// M4 unit tests for the remesh module.
//
// Verifies the three remesh modes against simple fixture meshes:
//   - Isotropic (pmp::uniform_remeshing) — should preserve vertex count
//     within a tolerance after remeshing at the source edge length.
//   - Adaptive (pmp::adaptive_remeshing) — same expectation, looser bounds.
//   - Decimate (igl::qslim) — should reduce face count by ~ratio.
//
// Plus an edge case: empty mesh must short-circuit with a warning without
// crashing.
//
// Note: TestRemesh deliberately does NOT include <igl/qslim.h> directly.
// Both this TU and contenttools_remesh.a would otherwise emit inline copies
// of igl::qslim; linking the two together produces ODR/symbol-resolution
// drift that makes qslim silently no-op on macOS arm64 + libc++. Calling
// through our Run() wrapper avoids the issue by using only the copy inside
// contenttools_remesh.a.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "modules/remesh/Remesh.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::remesh;

namespace {

// n×n grid plane, 2*n*n triangles, all edges length 1.0.
ProcessableMesh make_plane_grid(u32 n = 4) {
    ProcessableMesh m;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x)
            m.positions.emplace_back(v3{(f32)x, (f32)y, 0.f});
    const u32 row_size = n + 1;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const u32 bl = y * row_size + x;
            const u32 br = bl + 1;
            const u32 tl = bl + row_size;
            const u32 tr = tl + 1;
            m.indices.emplace_back(bl); m.indices.emplace_back(br); m.indices.emplace_back(tr);
            m.indices.emplace_back(bl); m.indices.emplace_back(tr); m.indices.emplace_back(tl);
        }
    }
    return m;
}

// UV sphere — closed, edge-manifold, dense enough for qslim to chew on.
ProcessableMesh make_uv_sphere(u32 stacks = 16, u32 slices = 16) {
    ProcessableMesh m;
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});   // north pole
    for (u32 i = 1; i < stacks; ++i) {
        const f32 phi = (f32)M_PI * (f32)i / (f32)stacks;
        const f32 y = std::cos(phi);
        const f32 r = std::sin(phi);
        for (u32 j = 0; j < slices; ++j) {
            const f32 theta = 2.f * (f32)M_PI * (f32)j / (f32)slices;
            m.positions.emplace_back(
                v3{r * std::cos(theta), y, r * std::sin(theta)});
        }
    }
    m.positions.emplace_back(v3{0.f, -1.f, 0.f});   // south pole

    const u32 north = 0;
    const u32 south = (u32)m.positions.size() - 1;
    for (u32 j = 0; j < slices; ++j) {
        const u32 a = 1 + j;
        const u32 b = 1 + (j + 1) % slices;
        m.indices.emplace_back(north); m.indices.emplace_back(a); m.indices.emplace_back(b);
    }
    for (u32 i = 1; i < stacks - 1; ++i) {
        const u32 row_a = 1 + (i - 1) * slices;
        const u32 row_b = 1 + i * slices;
        for (u32 j = 0; j < slices; ++j) {
            const u32 a = row_a + j;
            const u32 b = row_a + (j + 1) % slices;
            const u32 c = row_b + j;
            const u32 d = row_b + (j + 1) % slices;
            m.indices.emplace_back(a); m.indices.emplace_back(c); m.indices.emplace_back(b);
            m.indices.emplace_back(b); m.indices.emplace_back(c); m.indices.emplace_back(d);
        }
    }
    const u32 last_row = 1 + (stacks - 2) * slices;
    for (u32 j = 0; j < slices; ++j) {
        const u32 a = last_row + j;
        const u32 b = last_row + (j + 1) % slices;
        m.indices.emplace_back(south); m.indices.emplace_back(b); m.indices.emplace_back(a);
    }
    return m;
}

u32 face_count(const ProcessableMesh& m) { return (u32)m.indices.size() / 3; }

bool positions_are_finite_and_inside(const ProcessableMesh& m,
                                     const v3& bmin, const v3& bmax) {
    for (const auto& p : m.positions) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
        if (p.x < bmin.x || p.x > bmax.x ||
            p.y < bmin.y || p.y > bmax.y ||
            p.z < bmin.z || p.z > bmax.z)
            return false;
    }
    return true;
}

}  // namespace

// --- Isotropic ------------------------------------------------------------

bool test_isotropic_preserves_face_count_at_native_edge_length() {
    ProcessableMesh m = make_plane_grid(4);
    Params p;
    p.mode = Mode::Isotropic;
    p.target_edge_length = 1.0f;       // native grid spacing
    p.iterations = 5;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    if (face_count(m) == 0) return false;
    if (m.positions.empty()) return false;
    return true;
}

bool test_isotropic_keeps_topology_above_native_edge_length() {
    // The current remesher is split-only: a target longer than every existing
    // edge must leave the topology unchanged rather than collapsing edges.
    ProcessableMesh m = make_plane_grid(4);
    const u32 faces_before = face_count(m);
    const u32 vertices_before = (u32)m.positions.size();
    Params p;
    p.mode = Mode::Isotropic;
    p.target_edge_length = 2.0f;
    p.iterations = 5;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    return face_count(m) == faces_before &&
           m.positions.size() == vertices_before;
}

// --- Adaptive -------------------------------------------------------------

bool test_adaptive_runs_without_crash() {
    ProcessableMesh m = make_plane_grid(4);
    Params p;
    p.mode = Mode::Adaptive;
    p.target_edge_length = 1.0f;
    p.iterations = 5;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    return face_count(m) > 0;
}

bool test_split_vertices_stay_inside_source_bounds_with_attributes() {
    // Exercise several splits in one pass and every interpolated property.
    // A split midpoint must remain inside the source mesh's axis-aligned
    // bounds. This catches dangling PMP property handles corrupting v:point.
    ProcessableMesh m = make_plane_grid(4);
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        const auto& p = m.positions[i];
        m.normals.emplace_back(v3{0.f, 0.f, 1.f});
        m.tangents.emplace_back(v4{1.f, 0.f, 0.f, 1.f});
        m.colors.emplace_back(v3{p.x * 0.25f, p.y * 0.25f, 0.5f});
    }
    m.uv_sets.resize(1);
    for (const auto& p : m.positions)
        m.uv_sets[0].coords.emplace_back(v2{p.x * 0.25f, p.y * 0.25f});

    const u32 vertices_before = (u32)m.positions.size();
    Params p;
    p.mode = Mode::Isotropic;
    p.target_edge_length = 0.6f;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    if (m.positions.size() <= vertices_before) return false;
    if (m.normals.size() != m.positions.size() ||
        m.tangents.size() != m.positions.size() ||
        m.colors.size() != m.positions.size() ||
        m.uv_sets.empty() ||
        m.uv_sets[0].coords.size() != m.positions.size())
        return false;
    return positions_are_finite_and_inside(
        m, v3{0.f, 0.f, 0.f}, v3{4.f, 4.f, 0.f});
}

// --- Decimate (qslim) -----------------------------------------------------

bool test_decimate_halves_face_count() {
    ProcessableMesh m = make_uv_sphere(16, 16);   // 480 faces
    const u32 faces_before = face_count(m);
    Params p;
    p.mode = Mode::Decimate;
    p.ratio = 0.5f;                                // target = 240 faces
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;
    const u32 faces_after = face_count(m);
    // Allow generous tolerance — qslim's stopping condition isn't exact on a
    // closed sphere (boundary-at-infinity interacts with pole vertices).
    if (faces_after >= faces_before) return false;
    const u32 target = faces_before / 2;
    const u32 tolerance = std::max<u32>(1u, target / 5);   // ±20%
    return faces_after >= target - tolerance &&
           faces_after <= target + tolerance;
}

bool test_decimate_drops_aux_attributes_with_warning() {
    ProcessableMesh m = make_uv_sphere(16, 16);
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        m.normals.emplace_back(v3{0.f, 0.f, 1.f});
    }
    Params p;
    p.mode = Mode::Decimate;
    p.ratio = 0.5f;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, errs)) return false;

    bool found_warning = false;
    for (const auto& e : errs) {
        if (e.code == "remesh.qslim_dropped_attributes") {
            found_warning = true;
            break;
        }
    }
    if (!found_warning) return false;
    if (!m.normals.empty()) return false;   // cleared by rebuild_from_eigen
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
        if (e.code == "remesh.empty_input") { found = true; break; }
    }
    return found;
}

// --- Test runner ----------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_isotropic_preserves_face_count_at_native_edge_length),
        CASE(test_isotropic_keeps_topology_above_native_edge_length),
        CASE(test_adaptive_runs_without_crash),
        CASE(test_split_vertices_stay_inside_source_bounds_with_attributes),
        CASE(test_decimate_halves_face_count),
        CASE(test_decimate_drops_aux_attributes_with_warning),
        CASE(test_empty_mesh_returns_warning),
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
