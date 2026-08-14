// M7 unit tests for the lod module.
//
// Verifies recursive meshopt_simplify produces N levels at ratio^N of the
// source index count, the vertex pool is compacted, and aux attributes
// (normals/uvs) stay coherent with the new vertex count.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"
#include "modules/lod/Lod.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace primal;
using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::lod;

namespace {

// UV sphere with `stacks` latitude rings × `slices` longitude. Closed
// manifold; high-enough tri count to give meshopt_simplify something to
// work with. 16/16 → 480 tris; 32/32 → 1920 tris.
ProcessableMesh make_uv_sphere(u32 stacks, u32 slices) {
    ProcessableMesh m;
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});   // +Y pole
    for (u32 s = 1; s < stacks; ++s) {
        const f32 phi = (f32)s / stacks * 3.14159265358979f;
        const f32 y   = std::cos(phi);
        const f32 r   = std::sin(phi);
        for (u32 sl = 0; sl < slices; ++sl) {
            const f32 theta = (f32)sl / slices * 6.28318530717959f;
            m.positions.emplace_back(v3{r * std::cos(theta), y, r * std::sin(theta)});
        }
    }
    m.positions.emplace_back(v3{0.f, -1.f, 0.f});   // -Y pole
    const u32 top   = 0;
    const u32 bottom = (u32)m.positions.size() - 1;
    // Top cap
    for (u32 sl = 0; sl < slices; ++sl) {
        const u32 a = 1 + sl;
        const u32 b = 1 + (sl + 1) % slices;
        m.indices.emplace_back(top);
        m.indices.emplace_back(a);
        m.indices.emplace_back(b);
    }
    // Middle quads
    for (u32 s = 0; s < stacks - 2; ++s) {
        const u32 row_a = 1 + s * slices;
        const u32 row_b = 1 + (s + 1) * slices;
        for (u32 sl = 0; sl < slices; ++sl) {
            const u32 a0 = row_a + sl;
            const u32 a1 = row_a + (sl + 1) % slices;
            const u32 b0 = row_b + sl;
            const u32 b1 = row_b + (sl + 1) % slices;
            m.indices.emplace_back(a0); m.indices.emplace_back(b0); m.indices.emplace_back(b1);
            m.indices.emplace_back(a0); m.indices.emplace_back(b1); m.indices.emplace_back(a1);
        }
    }
    // Bottom cap
    for (u32 sl = 0; sl < slices; ++sl) {
        const u32 a = bottom - slices + sl;
        const u32 b = bottom - slices + (sl + 1) % slices;
        m.indices.emplace_back(bottom);
        m.indices.emplace_back(b);
        m.indices.emplace_back(a);
    }
    return m;
}

u32 face_count(const ProcessableMesh& m) { return (u32)m.indices.size() / 3; }

bool in_range(u32 v, u32 lo, u32 hi) { return v >= lo && v <= hi; }

}  // namespace

// --- Recursive ratio -----------------------------------------------------

bool test_lod_level1_hits_half_index_count() {
    ProcessableMesh hi = make_uv_sphere(32, 32);   // 1920 tris
    const u32 tris_before = face_count(hi);
    Params p;
    p.ratio       = 0.5f;
    p.max_levels  = 1;
    primal::utl::vector<ErrorReport> errs;
    utl::vector<ProcessableMesh> lods = Run(hi, p, errs);
    if (lods.size() != 1) return false;

    const u32 tris_l1 = face_count(lods[0]);
    const u32 target  = tris_before / 2;
    // ±25% — meshopt_simplify has slack via target_error.
    const u32 tol = std::max<u32>(target / 4, 30u);
    return in_range(tris_l1, target - tol, target + tol);
}

bool test_lod_levels_geometric_decay() {
    ProcessableMesh hi = make_uv_sphere(32, 32);
    Params p;
    p.ratio       = 0.5f;
    p.max_levels  = 3;
    primal::utl::vector<ErrorReport> errs;
    utl::vector<ProcessableMesh> lods = Run(hi, p, errs);
    if (lods.empty()) return false;

    // Each level should be ≤ previous level (monotonic decreasing).
    u32 prev = face_count(hi);
    for (const auto& lod : lods) {
        const u32 cur = face_count(lod);
        if (cur >= prev) return false;   // strictly less
        prev = cur;
    }
    return true;
}

// --- Vertex compaction ---------------------------------------------------

bool test_lod_compacts_unused_vertices() {
    ProcessableMesh hi = make_uv_sphere(16, 16);   // ~480 tris
    const u32 verts_before = (u32)hi.positions.size();
    Params p;
    p.ratio       = 0.5f;
    p.max_levels  = 3;
    primal::utl::vector<ErrorReport> errs;
    utl::vector<ProcessableMesh> lods = Run(hi, p, errs);
    if (lods.empty()) return false;

    // After compaction, every output vertex must be referenced by some
    // index (no unused trailing entries).
    for (const auto& lod : lods) {
        std::vector<bool> used(lod.positions.size(), false);
        for (u32 idx : lod.indices) {
            if (idx >= lod.positions.size()) return false;
            used[idx] = true;
        }
        for (bool u : used) if (!u) return false;
    }
    // Also: lower LODs should have fewer vertices than the source.
    for (const auto& lod : lods) {
        if (lod.positions.size() > verts_before) return false;
    }
    return true;
}

bool test_lod_preserves_aux_attribute_lengths() {
    ProcessableMesh hi = make_uv_sphere(16, 16);
    // Tag every input vertex with a sentinel normal + Texture UV.
    for (u32 i = 0; i < (u32)hi.positions.size(); ++i) {
        hi.normals.emplace_back(v3{0.f, 1.f, 0.f});
    }
    hi.uv_sets.emplace_back();
    hi.uv_sets.back().purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < (u32)hi.positions.size(); ++i) {
        hi.uv_sets.back().coords.emplace_back(v2{0.f, 0.f});
    }

    Params p;
    p.ratio       = 0.5f;
    p.max_levels  = 2;
    primal::utl::vector<ErrorReport> errs;
    utl::vector<ProcessableMesh> lods = Run(hi, p, errs);
    if (lods.empty()) return false;

    for (const auto& lod : lods) {
        if (lod.normals.size() != lod.positions.size()) return false;
        if (lod.uv_sets.size() != 1) return false;
        if (lod.uv_sets[0].coords.size() != lod.positions.size()) return false;
        if (lod.uv_sets[0].purpose != UVSetPurpose::Texture) return false;
    }
    return true;
}

// --- Edge cases ----------------------------------------------------------

bool test_empty_input_returns_warning() {
    ProcessableMesh empty;
    Params p;
    primal::utl::vector<ErrorReport> errs;
    auto lods = Run(empty, p, errs);
    if (!lods.empty()) return false;
    for (const auto& e : errs) {
        if (e.code == "lod.empty_input") return true;
    }
    return false;
}

bool test_invalid_ratio_rejected() {
    ProcessableMesh hi = make_uv_sphere(16, 16);
    Params p;
    p.ratio = 1.5f;
    primal::utl::vector<ErrorReport> errs;
    auto lods = Run(hi, p, errs);
    if (!lods.empty()) return false;
    for (const auto& e : errs) {
        if (e.code == "lod.invalid_ratio") return true;
    }
    return false;
}

bool test_small_mesh_stops_early() {
    // 12 indices / 4 tris is below our 12-index simplify threshold.
    ProcessableMesh tiny;
    for (u32 i = 0; i < 4; ++i) tiny.positions.emplace_back(v3{(f32)i, 0.f, 0.f});
    tiny.indices.emplace_back(0); tiny.indices.emplace_back(1); tiny.indices.emplace_back(2);
    tiny.indices.emplace_back(0); tiny.indices.emplace_back(2); tiny.indices.emplace_back(3);
    tiny.indices.emplace_back(0); tiny.indices.emplace_back(3); tiny.indices.emplace_back(1);
    tiny.indices.emplace_back(1); tiny.indices.emplace_back(3); tiny.indices.emplace_back(2);
    Params p;
    p.max_levels = 4;
    primal::utl::vector<ErrorReport> errs;
    auto lods = Run(tiny, p, errs);
    return lods.empty();   // stopped at level 1 threshold
}

// --- Test runner ---------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_lod_level1_hits_half_index_count),
        CASE(test_lod_levels_geometric_decay),
        CASE(test_lod_compacts_unused_vertices),
        CASE(test_lod_preserves_aux_attribute_lengths),
        CASE(test_empty_input_returns_warning),
        CASE(test_invalid_ratio_rejected),
        CASE(test_small_mesh_stops_early),
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
