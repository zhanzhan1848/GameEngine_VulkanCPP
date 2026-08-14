// M7 unit tests for the meshlet module.
//
// Verifies meshopt_buildMeshlets produces meshlets that respect the
// 256/128 cap, meshlet_vertices references stay in range, and bounds
// computation fills in center/radius/cone fields.

#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "modules/meshlet/Meshlet.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace primal;
using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::meshlet;

namespace {

// UV sphere — closed manifold, enough geometry to populate several meshlets.
ProcessableMesh make_uv_sphere(u32 stacks, u32 slices) {
    ProcessableMesh m;
    m.positions.emplace_back(v3{0.f, 1.f, 0.f});
    for (u32 s = 1; s < stacks; ++s) {
        const f32 phi = (f32)s / stacks * 3.14159265358979f;
        const f32 y   = std::cos(phi);
        const f32 r   = std::sin(phi);
        for (u32 sl = 0; sl < slices; ++sl) {
            const f32 theta = (f32)sl / slices * 6.28318530717959f;
            m.positions.emplace_back(v3{r * std::cos(theta), y, r * std::sin(theta)});
        }
    }
    m.positions.emplace_back(v3{0.f, -1.f, 0.f});
    const u32 top = 0, bottom = (u32)m.positions.size() - 1;
    for (u32 sl = 0; sl < slices; ++sl) {
        m.indices.emplace_back(top); m.indices.emplace_back(1 + sl); m.indices.emplace_back(1 + (sl + 1) % slices);
    }
    for (u32 s = 0; s < stacks - 2; ++s) {
        const u32 ra = 1 + s * slices, rb = 1 + (s + 1) * slices;
        for (u32 sl = 0; sl < slices; ++sl) {
            const u32 a0 = ra + sl, a1 = ra + (sl + 1) % slices;
            const u32 b0 = rb + sl, b1 = rb + (sl + 1) % slices;
            m.indices.emplace_back(a0); m.indices.emplace_back(b0); m.indices.emplace_back(b1);
            m.indices.emplace_back(a0); m.indices.emplace_back(b1); m.indices.emplace_back(a1);
        }
    }
    for (u32 sl = 0; sl < slices; ++sl) {
        m.indices.emplace_back(bottom);
        m.indices.emplace_back(bottom - slices + (sl + 1) % slices);
        m.indices.emplace_back(bottom - slices + sl);
    }
    return m;
}

}  // namespace

// --- Caps respected ------------------------------------------------------

bool test_meshlet_caps_256_128() {
    ProcessableMesh m = make_uv_sphere(16, 16);   // ~480 tris
    Params p;   // defaults 256/128
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, out, errs)) return false;
    if (out.meshlets.empty()) return false;
    for (const auto& ml : out.meshlets) {
        if (ml.vertex_count == 0 || ml.vertex_count > 256u) return false;
        if (ml.triangle_count == 0 || ml.triangle_count > 128u) return false;
    }
    return true;
}

bool test_meshlet_caps_custom_64_32() {
    ProcessableMesh m = make_uv_sphere(16, 16);
    Params p;
    p.max_vertices  = 64;
    p.max_triangles = 32;
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, out, errs)) return false;
    if (out.meshlets.empty()) return false;
    for (const auto& ml : out.meshlets) {
        if (ml.vertex_count > 64u)  return false;
        if (ml.triangle_count > 32u) return false;
    }
    // Smaller caps should yield more meshlets than defaults.
    Params p_default;
    MeshletData out_default;
    if (!Run(m, p_default, out_default, errs)) return false;
    return out.meshlets.size() >= out_default.meshlets.size();
}

// --- Triangle coverage ---------------------------------------------------

bool test_meshlet_triangle_count_matches_input() {
    ProcessableMesh m = make_uv_sphere(8, 8);
    const u32 tris_in = (u32)m.indices.size() / 3;
    Params p;
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, out, errs)) return false;
    u32 tris_out = 0;
    for (const auto& ml : out.meshlets) tris_out += ml.triangle_count;
    return tris_out == tris_in;
}

bool test_meshlet_vertex_refs_in_range() {
    ProcessableMesh m = make_uv_sphere(16, 16);
    const u32 nv = (u32)m.positions.size();
    Params p;
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, out, errs)) return false;
    for (const auto& ml : out.meshlets) {
        if (ml.vertex_offset + ml.vertex_count > out.meshlet_vertices.size()) return false;
        for (u32 i = 0; i < ml.vertex_count; ++i) {
            const u32 ref = out.meshlet_vertices[ml.vertex_offset + i];
            if (ref >= nv) return false;
        }
    }
    return true;
}

// --- Bounds computation --------------------------------------------------

bool test_meshlet_bounds_filled() {
    ProcessableMesh m = make_uv_sphere(16, 16);
    Params p;
    p.compute_bounds = true;
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    if (!Run(m, p, out, errs)) return false;
    for (const auto& ml : out.meshlets) {
        if (ml.radius <= 0.f) return false;       // sphere is non-degenerate
        if (!std::isfinite(ml.radius)) return false;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(ml.center[i]))    return false;
            if (!std::isfinite(ml.cone_apex[i])) return false;
            if (!std::isfinite(ml.cone_axis[i])) return false;
        }
        if (!std::isfinite(ml.cone_cutoff)) return false;
    }
    return true;
}

// --- Edge cases ----------------------------------------------------------

bool test_empty_mesh_returns_warning() {
    ProcessableMesh m;
    Params p;
    MeshletData out;
    primal::utl::vector<ErrorReport> errs;
    bool ok = Run(m, p, out, errs);
    if (ok) return false;
    if (!out.meshlets.empty()) return false;
    for (const auto& e : errs) {
        if (e.code == "meshlet.empty_input") return true;
    }
    return false;
}

// --- Test runner ---------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_meshlet_caps_256_128),
        CASE(test_meshlet_caps_custom_64_32),
        CASE(test_meshlet_triangle_count_matches_input),
        CASE(test_meshlet_vertex_refs_in_range),
        CASE(test_meshlet_bounds_filled),
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
